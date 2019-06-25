#ifndef BROWN_EXT_IST_LF_IMPL_H
#define BROWN_EXT_IST_LF_IMPL_H

#include "brown_ext_ist_lf.h"

#ifdef MEASURE_DURATION_STATS
class TimeThisScope {
private:
    bool condition;
    int tid;
    gstats_stat_id stat_id;
    uint64_t start;
public:
    TimeThisScope(const int _tid, gstats_stat_id _stat_id, bool _condition = true) {
        condition = _condition;
        if (condition) {
            tid = _tid;
            stat_id = _stat_id;
            start = get_server_clock();
        }
    }
    ~TimeThisScope() {
        if (condition) {
            auto duration = get_server_clock() - start;
            GSTATS_ADD(tid, stat_id, duration);
        }
    }
};
#endif

template <typename K, typename V, class Interpolate, class RecManager>
V istree<K,V,Interpolate,RecManager>::find(const int tid, const K& key) {
    assert(init[tid]);
    auto guard = recordmgr->getGuard(tid, true);
    casword_t ptr = prov->readPtr(tid, root->ptrAddr(0));
    assert(ptr);
    Node<K,V> * parent = root;
    int ixToPtr = 0;
    while (true) {
        if (unlikely(IS_KVPAIR(ptr))) {
            auto kv = CASWORD_TO_KVPAIR(ptr);
            if (kv->k == key) return kv->v;
            return NO_VALUE;
        } else if (unlikely(IS_REBUILDOP(ptr))) {
            auto rebuild = CASWORD_TO_REBUILDOP(ptr);
            ptr = NODE_TO_CASWORD(rebuild->rebuildRoot);
        } else if (IS_NODE(ptr)) {
            // ptr is an internal node
            parent = CASWORD_TO_NODE(ptr);
            assert(parent);
            ixToPtr = interpolationSearch(tid, key, parent);
            ptr = prov->readPtr(tid, parent->ptrAddr(ixToPtr));
        } else {
            assert(IS_VAL(ptr));
            assert(IS_EMPTY_VAL(ptr) || ixToPtr > 0); // invariant: leftmost pointer cannot contain a non-empty VAL (it contains a non-NULL pointer or an empty val casword)
            if (IS_EMPTY_VAL(ptr)) return NO_VALUE;
            auto v = CASWORD_TO_VAL(ptr);
            auto ixToKey = ixToPtr - 1;
            return (parent->key(ixToKey) == key) ? v : NO_VALUE;
        }
    }
}

template <typename K, typename V, class Interpolate, class RecManager>
void istree<K,V,Interpolate,RecManager>::helpFreeSubtree(const int tid, Node<K,V> * node) {
    // if node is the root of a *large* subtree (256+ children),
    // then have threads *collaborate* by reserving individual subtrees to free.
    // idea: reserve a subtree before freeing it by CASing it to NULL
    //       we are done when all pointers are NULL.
    
    // conceptually you reserve the right to reclaim everything under a node
    // (including the node) when you set its DIRTY_DIRTY_MARKED_FOR_FREE_MASK bit
    //
    // note: the dirty field doesn't exist for kvpair, value, empty value and rebuildop objects...
    // so to reclaim those if they are children of the root node passed to this function,
    // we claim the entire root node at the end, and go through those with one thread.
    
#ifdef USE_GSTATS
    TIMELINE_START(tid);
    DURATION_START(tid);
#endif
    
    // first, claim subtrees rooted at CHILDREN of this node
    // TODO: does this improve if we scatter threads in this iteration?
    for (int i=0;i<node->degree;++i) {
        auto ptr = prov->readPtr(tid, node->ptrAddr(i));
        if (IS_NODE(ptr)) {
            auto child = CASWORD_TO_NODE(ptr);
            if (child == NULL) continue;
            
            // claim subtree rooted at child
            while (true) {
                auto old = child->dirty;
                if (IS_DIRTY_MARKED_FOR_FREE(old)) break;
                if (CASB(&child->dirty, old, old | DIRTY_MARKED_FOR_FREE_MASK)) {
                    freeSubtree(tid, ptr, true, false);
                }
            }
        }
    }
    
    // then try to claim the node itself to handle special object types (kvpair, value, empty value, rebuildop).
    // claim node and its pointers that go to kvpair, value, empty value and rebuildop objects, specifically
    // (since those objects, and their descendents in the case of a rebuildop object,
    // are what remain unfreed [since all descendents of direct child *node*s have all been freed])
    while (true) {
        auto old = node->dirty;
        if (IS_DIRTY_MARKED_FOR_FREE(old)) break;
        if (CASB(&node->dirty, old, old | DIRTY_MARKED_FOR_FREE_MASK)) {
            // clean up pointers to non-*node* objects (and descendents of such objects)
            for (int i=0;i<node->degree;++i) {
                auto ptr = prov->readPtr(tid, node->ptrAddr(i));
                if (!IS_NODE(ptr)) {
                    freeSubtree(tid, ptr, true, false);
                }
            }
            freeNode(tid, node, true); // retire the ACTUAL node
        }
    }
    
#ifdef USE_GSTATS
    DURATION_END(tid, duration_traverseAndRetire);
    TIMELINE_END(tid, "freeSubtree");
#endif
}

template <typename K, typename V, class Interpolate, class RecManager>
size_t istree<K,V,Interpolate,RecManager>::markAndCount(const int tid, const casword_t ptr, bool tryTiming /* = true by default */) {
#ifdef MEASURE_DURATION_STATS
    TimeThisScope obj (tid, duration_markAndCount, tryTiming);
#endif
    
    if (unlikely(IS_KVPAIR(ptr))) return 1;
    if (unlikely(IS_VAL(ptr))) return 1 - IS_EMPTY_VAL(ptr);
    if (unlikely(IS_REBUILDOP(ptr))) {
        // if we are here seeing this rebuildop,
        // then we ALREADY marked the node that points to the rebuildop,
        // which means that rebuild op cannot possibly change that node
        // to effect the rebuilding.
        return markAndCount(tid, NODE_TO_CASWORD(CASWORD_TO_REBUILDOP(ptr)->rebuildRoot), false);
    }
    
    assert(IS_NODE(ptr));
    auto node = CASWORD_TO_NODE(ptr);
    
    // optimize by taking the sum from node->dirty if we run into a finished subtree
    auto result = node->dirty;
    if (IS_DIRTY_FINISHED(result)) return DIRTY_FINISHED_TO_SUM(result); // markAndCount has already FINISHED in this subtree, and sum is the count

    if (!IS_DIRTY_STARTED(result)) __sync_val_compare_and_swap(&node->dirty, 0, DIRTY_STARTED_MASK);
    
    // high level idea: if not at a leaf, try to divide work between any helpers at this node
    //      by using fetch&add to "soft-reserve" a subtree to work on.
    //      (each helper will get a different subtree!)
    // note that all helpers must still try to help ALL subtrees after, though,
    //      since a helper might crash after soft-reserving a subtree.
    //      the DIRTY_FINISHED indicator makes these final helping attempts more efficient.
    //
    // this entire idea of dividing work between helpers first can be disabled
    //      by defining IST_DISABLE_COLLABORATIVE_MARK_AND_COUNT
    //
    // can the clean fetch&add work division be adapted better for concurrent ideal tree construction?
    //
    // note: could i save a second traversal to build KVPair arrays by having
    //      each thread call addKVPair for each key it sees in THIS traversal?
    //      (maybe avoiding sort order issues by saving per-thread lists and merging)
    
#if !defined IST_DISABLE_COLLABORATIVE_MARK_AND_COUNT
    // optimize for contention by first claiming a subtree to recurse on
    // THEN after there are no more subtrees to claim, help (any that are still DIRTY_STARTED)
    if (node->degree > MAX_ACCEPTABLE_LEAF_SIZE) { // prevent this optimization from being applied at the leaves, where the number of fetch&adds will be needlessly high
        while (1) {
            auto ix = __sync_fetch_and_add(&node->nextMarkAndCount, 1);
            if (ix >= node->degree) break;
            markAndCount(tid, prov->readPtr(tid, node->ptrAddr(ix)), false);

            auto result = node->dirty;
            if (IS_DIRTY_FINISHED(result)) return DIRTY_FINISHED_TO_SUM(result); // markAndCount has already FINISHED in this subtree, and sum is the count
        }
    }
#endif
    
    // recurse over all subtrees
    size_t keyCount = 0;
    for (int i=0;i<node->degree;++i) {
        keyCount += markAndCount(tid, prov->readPtr(tid, node->ptrAddr(i)), false);
        
        auto result = node->dirty;
        if (IS_DIRTY_FINISHED(result)) return DIRTY_FINISHED_TO_SUM(result); // markAndCount has already FINISHED in this subtree, and sum is the count
    }
    
    __sync_bool_compare_and_swap(&node->dirty, DIRTY_STARTED_MASK, SUM_TO_DIRTY_FINISHED(keyCount));
    return keyCount;
}

template <typename K, typename V, class Interpolate, class RecManager>
void istree<K,V,Interpolate,RecManager>::addKVPairs(const int tid, casword_t ptr, IdealBuilder * b) {
    if (unlikely(IS_KVPAIR(ptr))) {
        auto pair = CASWORD_TO_KVPAIR(ptr);
        b->addKV(tid, pair->k, pair->v);
    } else if (unlikely(IS_REBUILDOP(ptr))) {
        auto op = CASWORD_TO_REBUILDOP(ptr);
        addKVPairs(tid, NODE_TO_CASWORD(op->rebuildRoot), b);
    } else {
        assert(IS_NODE(ptr));
        auto node = CASWORD_TO_NODE(ptr);
        assert(IS_DIRTY_FINISHED(node->dirty) && IS_DIRTY_STARTED(node->dirty));
        for (int i=0;i<node->degree;++i) {
            auto childptr = prov->readPtr(tid, node->ptrAddr(i));
            if (IS_VAL(childptr)) {
                if (IS_EMPTY_VAL(childptr)) continue;
                auto v = CASWORD_TO_VAL(childptr);
                assert(i > 0);
                auto k = node->key(i - 1); // it's okay that this read is not atomic with the value read, since keys of nodes do not change. (so, we can linearize the two reads when we read the value.)
                b->addKV(tid, k, v);
            } else {
                addKVPairs(tid, childptr, b);
            }
        }
    }
}

template <typename K, typename V, class Interpolate, class RecManager>
void istree<K,V,Interpolate,RecManager>::addKVPairsSubset(const int tid, RebuildOperation<K,V> * op, Node<K,V> * node, size_t * numKeysToSkip, size_t * numKeysToAdd, size_t depth, IdealBuilder * b, casword_t volatile * constructingSubtree) {
    for (int i=0;i<node->degree;++i) {
        if (*constructingSubtree != NODE_TO_CASWORD(NULL)) {
            GSTATS_ADD_IX(tid, num_bail_from_addkv_at_depth, 1, (depth > 9 ? 9 : depth));
            return; // stop early if someone else built the subtree already
        }
        
        assert(*numKeysToAdd > 0);
        assert(*numKeysToSkip >= 0);
        auto childptr = prov->readPtr(tid, node->ptrAddr(i));
        if (IS_VAL(childptr)) {
            if (IS_EMPTY_VAL(childptr)) {
                TRACE if (tid == 0) printf(" (e)");
                continue;
            }
            if (*numKeysToSkip > 0) {
                --*numKeysToSkip;
                TRACE if (tid == 0) printf(" (%lld)", (long long) CASWORD_TO_VAL(childptr));
            } else {
                assert(*numKeysToSkip == 0);
                auto v = CASWORD_TO_VAL(childptr);
                assert(i > 0);
                auto k = node->key(i - 1); // it's okay that this read is not atomic with the value read, since keys of nodes do not change. (so, we can linearize the two reads when we read the value.)
                b->addKV(tid, k, v);
                TRACE if (tid == 0) printf(" %lld", k);
                if (--*numKeysToAdd == 0) return;
            }
        } else if (unlikely(IS_KVPAIR(childptr))) {
            if (*numKeysToSkip > 0) {
                --*numKeysToSkip;
                TRACE if (tid == 0) printf(" (%lld)", CASWORD_TO_KVPAIR(childptr)->k);
            } else {
                assert(*numKeysToSkip == 0);
                auto pair = CASWORD_TO_KVPAIR(childptr);
                b->addKV(tid, pair->k, pair->v);
                TRACE if (tid == 0) printf(" (%lld)", pair->k);
                if (--*numKeysToAdd == 0) return;
            }
        } else if (unlikely(IS_REBUILDOP(childptr))) {
            auto child = CASWORD_TO_REBUILDOP(childptr)->rebuildRoot;
            assert(IS_DIRTY_FINISHED(child->dirty));
            auto childSize = DIRTY_FINISHED_TO_SUM(child->dirty);
            if (*numKeysToSkip < childSize) {
                addKVPairsSubset(tid, op, child, numKeysToSkip, numKeysToAdd, 1+depth, b, constructingSubtree);
                if (*numKeysToAdd == 0) return;
            } else {
                TRACE if (tid == 0) printf(" ([subtree containing %lld])", (long long) childSize);
                *numKeysToSkip -= childSize;
            }
        } else {
            assert(IS_NODE(childptr));
            auto child = CASWORD_TO_NODE(childptr);
            assert(IS_DIRTY_FINISHED(child->dirty));
            auto childSize = DIRTY_FINISHED_TO_SUM(child->dirty);
            if (*numKeysToSkip < childSize) {
                addKVPairsSubset(tid, op, child, numKeysToSkip, numKeysToAdd, 1+depth, b, constructingSubtree);
                if (*numKeysToAdd == 0) return;
            } else {
                TRACE if (tid == 0) printf(" ([subtree containing %lld])", (long long) childSize);
                *numKeysToSkip -= childSize;
            }
        }
    }
}

template<typename K, typename V, class Interpolate, class RecManager>
void istree<K, V, Interpolate, RecManager>::subtreeBuildAndReplace(const int tid, RebuildOperation<K,V>* op, Node<K,V>* parent, size_t ix, size_t childSize, size_t remainder) {
#ifdef USE_GSTATS
    DURATION_START(tid);
#endif
    
    // compute initSize of new subtree
    auto totalSizeSoFar = ix*childSize + (ix < remainder ? ix : remainder);
    auto newChildSize = childSize + (ix < remainder);
    
    // build new subtree
    IdealBuilder b (this, newChildSize, 1+op->depth);
    auto numKeysToSkip = totalSizeSoFar;
    auto numKeysToAdd = newChildSize;
    TRACE printf("    tid=%d calls addKVPairsSubset with numKeysToSkip=%lld and numKeysToAdd=%lld\n", tid, (long long) numKeysToSkip, (long long) numKeysToAdd);
    TRACE printf("    tid=%d visits keys", tid);
    addKVPairsSubset(tid, op, op->rebuildRoot, &numKeysToSkip, &numKeysToAdd, op->depth, &b, parent->ptrAddr(ix)); // construct the subtree
    TRACE printf("\n");
    if (parent->ptr(ix) != NODE_TO_CASWORD(NULL)) {
        GSTATS_ADD_IX(tid, num_bail_from_addkv_at_depth, 1, op->depth);
#ifdef USE_GSTATS
        DURATION_END(tid, duration_wastedWorkBuilding);
#endif
        return;
    }
    auto ptr = b.getCASWord(tid, parent->ptrAddr(ix));
    if (NODE_TO_CASWORD(NULL) == ptr) {
#ifdef USE_GSTATS
        DURATION_END(tid, duration_wastedWorkBuilding);
#endif
        return; // if we didn't build a tree, because someone else already replaced this subtree, then we just stop here (just avoids an unnecessary cas below in this case; apart from this cas, which will fail, the behaviour is no different whether we return here or execute the following...)
    }

    // try to attach new subtree
    if (ix > 0) *parent->keyAddr(ix-1) = b.getMinKey();
    if (__sync_bool_compare_and_swap(parent->ptrAddr(ix), NODE_TO_CASWORD(NULL), ptr)) { // try to CAS the subtree in to the new root we are building (consensus to decide who built it)
        TRACE printf("    tid=%d successfully CASs newly build subtree\n", tid);
        // success
    } else {
        TRACE printf("    tid=%d fails to CAS newly build subtree\n", tid);
        freeSubtree(tid, ptr, false);
#ifdef USE_GSTATS
        DURATION_END(tid, duration_wastedWorkBuilding);
#endif
    }
    assert(prov->readPtr(tid, parent->ptrAddr(ix)));
}

template <typename K, typename V, class Interpolate, class RecManager>
casword_t istree<K,V,Interpolate,RecManager>::createIdealConcurrent(const int tid, RebuildOperation<K,V> * op, const size_t keyCount) {
    // Note: the following could be encapsulated in a ConcurrentIdealBuilder class
    
    TRACE printf("createIdealConcurrent(tid=%d, rebuild op=%llx, keyCount=%lld)\n", tid, (unsigned long long) op, (long long) keyCount);

    if (unlikely(keyCount == 0)) return EMPTY_VAL_TO_CASWORD;
    
    double numChildrenD = std::sqrt((double) keyCount);
    size_t numChildren = (size_t) std::ceil(numChildrenD);
    size_t childSize = keyCount / (size_t) numChildren;
    size_t remainder = keyCount % numChildren;
    // remainder is the number of children with childSize+1 pair subsets
    // (the other (numChildren - remainder) children have childSize pair subsets)
    TRACE printf("    tid=%d numChildrenD=%f numChildren=%lld childSize=%lld remainder=%lld\n", tid, numChildrenD, (long long) numChildren, (long long) childSize, (long long) remainder);
    
    casword_t word = NODE_TO_CASWORD(NULL);
    casword_t newRoot = op->newRoot;
    if (newRoot == EMPTY_VAL_TO_CASWORD) {
        return NODE_TO_CASWORD(NULL);
    } else if (newRoot != NODE_TO_CASWORD(NULL)) {
        word = newRoot;
        TRACE printf("    tid=%d used existing op->newRoot=%llx\n", tid, (unsigned long long) op->newRoot);
    } else {
        assert(newRoot == NODE_TO_CASWORD(NULL));
        
        if (keyCount <= MAX_ACCEPTABLE_LEAF_SIZE) {
            IdealBuilder b (this, keyCount, op->depth);
            casword_t dummy = NODE_TO_CASWORD(NULL);
            addKVPairs(tid, NODE_TO_CASWORD(op->rebuildRoot), &b);
            word = b.getCASWord(tid, &dummy);
            assert(word != NODE_TO_CASWORD(NULL));
        } else {
#ifndef IST_DISABLE_MULTICOUNTER_AT_ROOT
            if (op->depth <= 1) {
                word = NODE_TO_CASWORD(createMultiCounterNode(tid, numChildren));
                TRACE printf("    tid=%d create multi counter root=%llx\n", tid, (unsigned long long) word);
            } else {
#endif
                word = NODE_TO_CASWORD(createNode(tid, numChildren));
                TRACE printf("    tid=%d create regular root=%llx\n", tid, (unsigned long long) word);
#ifndef IST_DISABLE_MULTICOUNTER_AT_ROOT
            }
#endif

            CASWORD_TO_NODE(word)->degree = CASWORD_TO_NODE(word)->capacity; // to appease debug asserts (which state that we never go out of degree bounds on pointer/key accesses)
            for (int i=0;i<CASWORD_TO_NODE(word)->capacity;++i) {
                *CASWORD_TO_NODE(word)->ptrAddr(i) = NODE_TO_CASWORD(NULL);
            }
            CASWORD_TO_NODE(word)->degree = 0; // zero this out so we can have threads synchronize a bit later by atomically incrementing this until it hits node->capacity
        }
        
        // try to CAS node into the RebuildOp
        if (__sync_bool_compare_and_swap(&op->newRoot, NODE_TO_CASWORD(NULL), word)) { // this should (and will) fail if op->newRoot == EMPTY_VAL_TO_CASWORD because helping is done
            TRACE printf("    tid=%d CAS'd op->newRoot successfully\n", tid);
            assert(word != NODE_TO_CASWORD(NULL));
            //assert(word == op->newRoot); // not a valid assert, since we could change from NODE to EMPTY_VAL!
            // success; node == op->newRoot will be built by us and possibly by helpers
        } else {
            TRACE printf("    tid=%d failed to CAS op->newRoot\n", tid);
            
            // we failed the newRoot CAS, so we lost the consensus race.
            // someone else CAS'd their newRoot in, so ours is NOT the new root.
            // reclaim ours, and help theirs instead.
            freeSubtree(tid, word, false);
            
            // try to help theirs
            word = op->newRoot;
            assert(word != NODE_TO_CASWORD(NULL));
            //assert(word == op->newRoot); // not a valid assert, since earlier read can be a new node, and this read can be EMPTY_VAL!
            if (word == EMPTY_VAL_TO_CASWORD) {
                // this rebuildop was part of a subtree that was rebuilt,
                // and someone else CAS'd the newRoot from non-null to "null" (empty val)
                // (as part of reclamation) *after* we performed our CAS above.
                // at any rate, we no longer need to help.
                
                // TODO: i forget now how this interacts with reclamation?
                //      need to re-conceptualize the algorithm in its entirety!
                // IIRC, op->newRoot can only transition from CASWORD(NULL) to CASWORD(node) to CASWORD_EMPTYVAL
                //      (the final state meaning the new root / subtree(?) was *reclaimed*)
                // QUESTION: how can this safely be reclaimed while we have a pointer to it? shouldn't EBR stop this?
                
                assert(IS_DIRTY_STARTED(op->parent->dirty));
                return NODE_TO_CASWORD(NULL);
            }
        }
    }
    assert(word != NODE_TO_CASWORD(NULL));
    assert(op->newRoot != NODE_TO_CASWORD(NULL));
    assert(op->newRoot == word || EMPTY_VAL_TO_CASWORD /* as per above, rebuildop was part of a subtree that was rebuilt, and "word" was reclaimed! */);
    
    // stop here if there is no subtree to build (just one kvpair or node)
    if (IS_KVPAIR(word) || keyCount <= MAX_ACCEPTABLE_LEAF_SIZE) return word;
    
    assert(IS_NODE(word));
    auto node = CASWORD_TO_NODE(word);
    
#ifndef NDEBUG
    if (node->capacity != numChildren) {
        printf("keyCount=%lld node->capacity=%lld numChildren=%lld numChildrenD=%f childSize=%lld remainder=%lld op->depth=%lld word=%llx\n", keyCount, node->capacity, numChildren, numChildrenD, childSize, remainder, op->depth, word);
    }
#endif
    assert(node->capacity == numChildren);
    
    // opportunistically try to build different subtrees from any other concurrent threads
    // by synchronizing via node->degree. concurrent threads increment node->degree using cas
    // to "reserve" a subtree to work on (not truly exclusively---still a lock-free mechanism).
    TRACE printf("    tid=%d starting to build subtrees\n", tid);
    while (1) {
        auto ix = node->degree;
        if (ix >= node->capacity) break;                                        // skip to the helping phase if all subtrees are already being constructed
        if (__sync_bool_compare_and_swap(&node->degree, ix, 1+ix)) {            // use cas to soft-reserve a subtree to construct
            TRACE printf("    tid=%d incremented degree from %lld\n", tid, (long long) ix);
            subtreeBuildAndReplace(tid, op, node, ix, childSize, remainder);
        }
    }
    
    // try to help complete subtree building if necessary
    // (partially for lock-freedom, and partially for performance)

    // help linearly starting at a random position (to probabilistically scatter helpers)
    // TODO: determine if helping starting at my own thread id would help? or randomizing my chosen subtree every time i want to help one? possibly help according to a random permutation?
    //printf("tid=%d myRNG=%lld\n", tid, (size_t) myRNG);
    assert(init[tid]);
    auto ix = threadRNGs[tid].next(numChildren); //myRNG->next(numChildren);
    for (int __i=0;__i<numChildren;++__i) {
        auto i = (__i+ix) % numChildren;
        if (prov->readPtr(tid, node->ptrAddr(i)) == NODE_TO_CASWORD(NULL)) {
            subtreeBuildAndReplace(tid, op, node, i, childSize, remainder);
            GSTATS_ADD(tid, num_help_subtree, 1);
        }
    }

    node->initSize = keyCount;
    node->minKey = node->key(0);
    node->maxKey = node->key(node->degree-2);
    assert(node->minKey != INF_KEY);
    assert(node->maxKey != INF_KEY);
    assert(node->minKey <= node->maxKey);
    return word;
}

template <typename K, typename V, class Interpolate, class RecManager>
void istree<K,V,Interpolate,RecManager>::helpRebuild(const int tid, RebuildOperation<K,V> * op) {
#ifdef USE_GSTATS
    TIMELINE_START_C(tid, (op->depth < 1));
#endif
    
#ifdef MEASURE_REBUILDING_TIME
//    GSTATS_TIMER_RESET(tid, timer_rebuild);
    GSTATS_ADD(tid, num_help_rebuild, 1);
#endif
    
//    assert(!recordmgr->isQuiescent(tid));
    auto keyCount = markAndCount(tid, NODE_TO_CASWORD(op->rebuildRoot));
    auto oldWord = REBUILDOP_TO_CASWORD(op);
    
#ifdef IST_DISABLE_REBUILD_HELPING
    {
#ifdef USE_GSTATS
        DURATION_START(tid);
#endif
        if (__sync_bool_compare_and_swap(&op->debug_sync_in_experimental_no_collaboration_version, 0, 1)) {
            // continue; you are the chosen one to rebuild the tree
        } else {
            // you are not the chosen one. you are not the rebuilder.
            while (op->debug_sync_in_experimental_no_collaboration_version == 1) {
                // minor experimental hack: just WAIT until op is replaced
                // (no point "helping" (duplicating work) to facilitate a true simulation
                //  of the lock-free no-collaboration algorithm in this case, since it
                //  won't change results at all. no extra parallelism or performance is
                //  gained by having n threads duplicate efforts rebuilding the entire
                //  tree until exactly one succeeds. in practice there are no thread
                //  crashes, and no major delays. in fact, the real lock-free algorithm
                //  performs WORSE than this version, since there is a high cost for
                //  allocating MANY tree nodes that are doomed to be useless, and will
                //  subsequently need to be freed. so, the experiments will simply
                //  *underestimate* the benefit of our collaborative rebuilding alg.)
            }
#ifdef USE_GSTATS
            DURATION_END(tid, duration_wastedWorkBuilding);
#endif
            return;
        }
    }
#endif

    
#ifdef USE_GSTATS
    DURATION_START(tid);
#endif
    casword_t newWord = createIdealConcurrent(tid, op, keyCount);
    if (newWord == NODE_TO_CASWORD(NULL)) {
#ifdef USE_GSTATS
        DURATION_END(tid, duration_buildAndReplace);
#endif
#ifdef IST_DISABLE_REBUILD_HELPING
        op->debug_sync_in_experimental_no_collaboration_version = 2;
#endif
        return; // someone else already *finished* helping
        
        // TODO: help to free old subtree?
    }
    auto result = prov->dcssPtr(tid, (casword_t *) &op->parent->dirty, 0, (casword_t *) op->parent->ptrAddr(op->index), oldWord, newWord).status;
    if (result == DCSS_SUCCESS) {
        SOFTWARE_BARRIER;
        assert(op->success == false);
        op->success = true;
        SOFTWARE_BARRIER;
        GSTATS_ADD_IX(tid, num_complete_rebuild_at_depth, 1, op->depth);
//        freeSubtree(tid, NODE_TO_CASWORD(op->rebuildRoot), true);
//        helpFreeSubtree(tid, op->rebuildRoot);
        recordmgr->retire(tid, op); // note: it's okay to retire this before reading op-> fields below!!! (this is because retire means don't deallocate until AFTER our memory guard section)
    } else {
        // if we fail to CAS, then either:
        // 1. someone else CAS'd exactly newWord into op->parent->ptrAddr(op->index), or
        // 2. this rebuildop is part of a subtree that is marked and rebuilt by another rebuildop,
        //    and this DCSS failed because op->parent->dirty == 1.
        //    in this case, we should try to reclaim the subtree at newWord.
        //
        if (result == DCSS_FAILED_ADDR1) {
            // [[failed because dirty (subsumed by another rebuild operation)]]
            // note: a rebuild operation should almost never be subsumed by one started higher up,
            // because it's unlikely that while we are trying to
            // rebuild one subtree another rebuild just so happens to start above
            // (since one will only start if it was ineligible to start when we began our own reconstruction,
            //  then enough operations are performed to make a higher tree eligible for rebuild,
            //  then we finish our own rebuilding and try to DCSS our new subtree in)
            // to test this: let's measure whether this happens...
            // apparently it does happen... in a 100% update workload for 15sec with 192 threads, we have: sum rebuild_is_subsumed_at_depth by_index=0 210 1887 277 5 
            //      these numbers represent how many subsumptions happened at each depth (none at depth 0 (impossible), 210 at depth 1, and so on).
            //      regardless, this is not a performance issue for now. (at most 3 of these calls took 10ms+; the rest were below that threshold.)
            //      *if* it becomes an issue then helpFreeSubtree or something like it should fix the problem.
            
            GSTATS_ADD(tid, rebuild_is_subsumed, 1); // if this DOES happen, it will be very expensive (well, *if* it's at the top of the tree...) because ONE thread will do it, and this will delay epoch advancement greatly
            GSTATS_ADD_IX(tid, rebuild_is_subsumed_at_depth, 1, op->depth);
            
            // try to claim the NEW subtree located at op->newWord for reclamation
            if (op->newRoot != NODE_TO_CASWORD(NULL)
                    && __sync_bool_compare_and_swap(&op->newRoot, newWord, EMPTY_VAL_TO_CASWORD)) {
                freeSubtree(tid, newWord, true);
                // note that other threads might be trying to help our rebuildop,
                // and so might be accessing the subtree at newWord.
                // so, we use retire rather than deallocate.
            }
            // otherwise, someone else reclaimed the NEW subtree
            assert(op->newRoot == EMPTY_VAL_TO_CASWORD);
        } else {
            assert(result == DCSS_FAILED_ADDR2);
        }
    }
#ifdef USE_GSTATS
    DURATION_END(tid, duration_buildAndReplace);
#endif
    
//#ifdef MEASURE_REBUILDING_TIME
//    auto cappedDepth = std::min((size_t) 9, op->depth);
//    GSTATS_ADD_IX(tid, elapsed_rebuild_depth, GSTATS_TIMER_ELAPSED(tid, timer_rebuild), cappedDepth);
//#endif
    
#ifdef USE_GSTATS
    TIMELINE_END_C("helpRebuild", tid, (op->depth < 1));
#endif
    
    // collaboratively free the old subtree, if appropriate (if it was actually replaced)
    if (op->success) {
        if (op->rebuildRoot->degree < 256) {
            if (result == DCSS_SUCCESS) {
                // this thread was the one whose DCSS operation performed the actual swap
                freeSubtree(tid, NODE_TO_CASWORD(op->rebuildRoot), true);
            }
        } else {
#ifdef IST_DISABLE_COLLABORATIVE_FREE_SUBTREE
            if (result == DCSS_SUCCESS) freeSubtree(tid, NODE_TO_CASWORD(op->rebuildRoot), true);
#else
            helpFreeSubtree(tid, op->rebuildRoot);
#endif
        }
    }
    
#ifdef IST_DISABLE_REBUILD_HELPING
        op->debug_sync_in_experimental_no_collaboration_version = 2;
#endif
}

template <typename K, typename V, class Interpolate, class RecManager>
void istree<K,V,Interpolate,RecManager>::rebuild(const int tid, Node<K,V> * rebuildRoot, Node<K,V> * parent, int indexOfRebuildRoot /* in parent */, const size_t depth) {
//    assert(!recordmgr->isQuiescent(tid));
    auto op = new RebuildOperation<K,V>(rebuildRoot, parent, indexOfRebuildRoot, depth);
    auto ptr = REBUILDOP_TO_CASWORD(op);
    auto old = NODE_TO_CASWORD(op->rebuildRoot);
    assert(op->parent == parent);
    auto result = prov->dcssPtr(tid, (casword_t *) &op->parent->dirty, 0, (casword_t *) op->parent->ptrAddr(op->index), old, ptr).status;
    if (result == DCSS_SUCCESS) {
        helpRebuild(tid, op);
    } else {
        // in this case, we have exclusive access to free op.
        // this is because we are the only ones who will try to perform a DCSS to insert op into the data structure.
        assert(result == DCSS_FAILED_ADDR1 || result == DCSS_FAILED_ADDR2);
        recordmgr->deallocate(tid, op);
    }
}

template <typename K, typename V, class Interpolate, class RecManager>
int istree<K,V,Interpolate,RecManager>::interpolationSearch(const int tid, const K& key, Node<K,V> * const node) {
    __builtin_prefetch(&node->minKey, 1);
    __builtin_prefetch(&node->maxKey, 1);
    
    // these next 3 prefetches are shockingly effective... 20% performance boost in some large scale search-only workloads... (reducing L3 cache misses by 2-3 per search...)
    __builtin_prefetch((node->keyAddr(0)), 1);
    __builtin_prefetch((node->keyAddr(0))+(8), 1);
    __builtin_prefetch((node->keyAddr(0))+(16), 1);

    auto deg = node->degree;
    
    //assert(node->degree >= 1);
    if (unlikely(deg == 1)) {
//        GSTATS_APPEND_D(tid, visited_in_isearch, 1);
        return 0;
    }
    
    const int numKeys = deg - 1;
    const K& minKey = node->minKey;
    const K& maxKey = node->maxKey;
    
    if (unlikely(key < minKey)) {
//        GSTATS_APPEND_D(tid, visited_in_isearch, 1);
        return 0;
    }
    if (unlikely(key >= maxKey)) {
//        GSTATS_APPEND_D(tid, visited_in_isearch, 1);
        return numKeys;
    }
    // assert: minKey <= key < maxKey
    int ix = (numKeys * (key - minKey) / (maxKey - minKey));

    __builtin_prefetch((node->keyAddr(0))+(ix-8), 1);                           // prefetch approximate key location
    __builtin_prefetch((node->keyAddr(0))+(ix), 1);                             // prefetch approximate key location
    __builtin_prefetch((node->keyAddr(0))+(ix+8), 1);                           // prefetch approximate key location
    
    const K& ixKey = node->key(ix);
//    std::cout<<"key="<<key<<" minKey="<<minKey<<" maxKey="<<maxKey<<" ix="<<ix<<" ixKey="<<ixKey<<std::endl;
    if (key < ixKey) {
        // search to the left for node.key[i] <= key, then return i+1
        int i;
        for (i=ix-1;i>=0;--i) {
            if (unlikely(key >= node->key(i))) {
//                GSTATS_APPEND_D(tid, visited_in_isearch, (ix-1) - i + 1);
                return i+1;
            }
        }
        assert(false);
    } else if (key > ixKey) {
        int i;
        for (i=ix+1;i<numKeys;++i) { // recall: degree - 1 keys vs degree pointers
            if (unlikely(key < node->key(i))) {
//                GSTATS_APPEND_D(tid, visited_in_isearch, i - (ix+1) + 1);
                return i;
            }
        }
        assert(false);
    } else {
//        GSTATS_APPEND_D(tid, visited_in_isearch, 1);
        return ix+1;
    }
}

// note: val is unused if t == Erase
template <typename K, typename V, class Interpolate, class RecManager>
V istree<K,V,Interpolate,RecManager>::doUpdate(const int tid, const K& key, const V& val, UpdateType t) {
    assert(init[tid]);
//    const double SAMPLING_PROB = 1.;
    const int MAX_PATH_LENGTH = 64; // in practice, the depth is probably less than 10 even for many billions of keys. max is technically nthreads + O(log log n), but this requires an astronomically unlikely event.
    Node<K,V> * path[MAX_PATH_LENGTH]; // stack to save the path
    int pathLength;
    Node<K,V> * node;

retry:
    pathLength = 0;
    auto guard = recordmgr->getGuard(tid);
    node = root;
    while (true) {
        auto ix = interpolationSearch(tid, key, node); // search INSIDE one node
retryNode:
        bool affectsChangeSum = true;
        auto word = prov->readPtr(tid, node->ptrAddr(ix));
        if (IS_KVPAIR(word) || IS_VAL(word)) {
            KVPair<K,V> * pair = NULL;
            Node<K,V> * newNode = NULL;
            KVPair<K,V> * newPair = NULL;
            auto newWord = (casword_t) NULL;

            assert(IS_EMPTY_VAL(word) || !IS_VAL(word) || ix > 0);
            auto foundKey = INF_KEY;
            auto foundVal = NO_VALUE;
            if (IS_VAL(word)) {
                foundKey = (IS_EMPTY_VAL(word)) ? INF_KEY : foundKey = node->key(ix - 1);
                if (!IS_EMPTY_VAL(word)) foundVal = CASWORD_TO_VAL(word);
            } else {
                assert(IS_KVPAIR(word));
                pair = CASWORD_TO_KVPAIR(word);
                foundKey = pair->k;
                foundVal = pair->v;
            }
            assert(foundVal == NO_VALUE || foundVal > 0 && foundKey < INF_KEY/8); // value must have top 3 bits empty so we can shift it
            
            if (foundKey == key) {
                if (t == InsertReplace) {
                    newWord = VAL_TO_CASWORD(val);
                    if (foundVal != NO_VALUE) affectsChangeSum = false; // note: should NOT count towards changeSum, because it cannot affect the complexity of operations
                } else if (t == InsertIfAbsent) {
                    if (foundVal != NO_VALUE) return foundVal;
                    newWord = VAL_TO_CASWORD(val);
                } else {
                    assert(t == Erase);
                    if (foundVal == NO_VALUE) return NO_VALUE;
                    newWord = EMPTY_VAL_TO_CASWORD;
                }
            } else {
                if (t == InsertReplace || t == InsertIfAbsent) {
                    if (foundVal == NO_VALUE) {
                        // after the insert, this pointer will lead to only one kvpair in the tree,
                        // so we just create a kvpair instead of a node
                        newPair = createKVPair(tid, key, val);
                        newWord = KVPAIR_TO_CASWORD(newPair);
                    } else {
                        // there would be 2 kvpairs, so we create a node
                        KVPair<K,V> pairs[2];
                        if (key < foundKey) {
                            pairs[0] = { key, val };
                            pairs[1] = { foundKey, foundVal };
                        } else {
                            pairs[0] = { foundKey, foundVal };
                            pairs[1] = { key, val };
                        }
                        newNode = createLeaf(tid, pairs, 2);
                        newWord = NODE_TO_CASWORD(newNode);
                        foundVal = NO_VALUE; // the key we are inserting had no current value
                    }
                } else {
                    assert(t == Erase);
                    return NO_VALUE;
                }
            }
            assert(newWord);
            assert((newWord & (~TOTAL_MASK)));
            
            // DCSS that performs the update
            assert(ix >= 0);
            assert(ix < node->degree);
            auto result = prov->dcssPtr(tid, (casword_t *) &node->dirty, 0, (casword_t *) node->ptrAddr(ix), word, newWord);
            switch (result.status) {
                case DCSS_FAILED_ADDR2: // retry from same node
                    if (newPair) recordmgr->deallocate(tid, newPair);
                    if (newNode) freeNode(tid, newNode, false);
                    goto retryNode;
                    break;
                case DCSS_FAILED_ADDR1: // node is dirty; retry from root
                    if (newPair) recordmgr->deallocate(tid, newPair);
                    if (newNode) freeNode(tid, newNode, false);
                    goto retry;
                    break;
                case DCSS_SUCCESS:
                    //assert(!recordmgr->isQuiescent(tid));
                    if (pair) recordmgr->retire(tid, pair);
                    
                    if (!affectsChangeSum) break;
                    
//                    // probabilistically increment the changeSums of ancestors
//                    if (myRNG->next() / (double) std::numeric_limits<uint64_t>::max() < SAMPLING_PROB) {
                        for (int i=0;i<pathLength;++i) {
                            path[i]->incrementChangeSum(tid, &threadRNGs[tid]); // myRNG);
                        }
//                    }
                    
                    // now, we must determine whether we should rebuild
                    for (int i=0;i<pathLength;++i) {
//                        if ((path[i]->changeSum + (NUM_PROCESSES-1)) / (SAMPLING_PROB * (1 - EPS)) >= REBUILD_FRACTION * path[i]->initSize) {
                        if (path[i]->readChangeSum(tid, &threadRNGs[tid] /*myRNG*/) >= REBUILD_FRACTION * path[i]->initSize) {
                            if (i == 0) {
#ifndef NO_REBUILDING
                                GSTATS_ADD_IX(tid, num_try_rebuild_at_depth, 1, 0);
                                rebuild(tid, path[0], root, 0, 0);
#endif
                            } else {
                                auto parent = path[i-1];
                                assert(parent->degree > 1);
                                assert(path[i]->degree > 1);
                                auto index = interpolationSearch(tid, path[i]->key(0), parent);

#ifndef NDEBUG
#ifdef TOTAL_THREADS
                                // single threaded only debug info
                                if (path[i]->degree == 1 || (TOTAL_THREADS == 1 && CASWORD_TO_NODE(parent->ptr(index)) != path[i])) {
                                    std::cout<<"i="<<i<<std::endl;
                                    std::cout<<"path length="<<pathLength<<std::endl;
                                    std::cout<<"parent@"<<(size_t) parent<<std::endl;
                                    std::cout<<"parent->degree="<<parent->degree<<std::endl;
                                    std::cout<<"parent keys";
                                    for (int j=0;j<parent->degree - 1;++j) std::cout<<" "<<parent->key(j);
                                    std::cout<<std::endl;
                                    std::cout<<"parent ptrs (converted)";
                                    for (int j=0;j<parent->degree;++j) std::cout<<" "<<(parent->ptr(j) & ~TOTAL_MASK);
                                    std::cout<<std::endl;
                                    std::cout<<"index="<<index<<std::endl;
                                    std::cout<<"parent->ptr(index) (converted)="<<(parent->ptr(index) & ~TOTAL_MASK)<<std::endl;
                                    std::cout<<"path[i]@"<<(size_t) path[i]<<std::endl;
                                    std::cout<<"path[i]->degree="<<path[i]->degree<<std::endl;
                                    std::cout<<"path[i]->key(0)="<<(path[i]->degree > 1 ? path[i]->key(0) : -1)<<std::endl;
                                    std::cout<<"path[i]->ptr(0)="<<(size_t) path[i]->ptr(0)<<std::endl;
                                    std::cout<<"path[i]->ptr(0) (converted)="<<(path[i]->ptr(0) & ~TOTAL_MASK)<<std::endl;
                                    std::cout<<"newWord="<<newWord<<std::endl;
                                    std::cout<<"newWord (converted)="<<(newWord & ~TOTAL_MASK)<<std::endl;
                                    if (IS_KVPAIR(newWord)) {
                                        std::cout<<"new key="<<CASWORD_TO_KVPAIR(newWord)->k<<std::endl;
                                    }
                                    std::cout<<"foundVal="<<foundVal<<std::endl;
                                    assert(false);
                                }
#endif
#endif
                                
#ifndef NO_REBUILDING
                                GSTATS_ADD_IX(tid, num_try_rebuild_at_depth, 1, i);
                                rebuild(tid, path[i], parent, index, i);
#endif
                            }
                            break;
                        }
                    }
                    break;
                default:
                    setbench_error("impossible switch case");
                    break;
            }
            return foundVal;
        } else if (IS_REBUILDOP(word)) {
            //std::cout<<"found supposed rebuildop "<<(size_t) word<<" at path length "<<pathLength<<std::endl;
            helpRebuild(tid, CASWORD_TO_REBUILDOP(word));
            goto retry;
        } else {
            assert(IS_NODE(word));
            node = CASWORD_TO_NODE(word);
            path[pathLength++] = node; // push on stack
            assert(pathLength <= MAX_PATH_LENGTH);
        }
    }
}

template <typename K, typename V, class Interpolate, class RecManager>
Node<K,V>* istree<K,V,Interpolate,RecManager>::createNode(const int tid, const int degree) {
    size_t sz = sizeof(Node<K,V>) + sizeof(K) * (degree - 1) + sizeof(casword_t) * degree;
    Node<K,V> * node = (Node<K,V> *) ::operator new (sz); //(Node<K,V> *) new char[sz];
//    std::cout<<"node of degree "<<degree<<" allocated size "<<sz<<" @ "<<(size_t) node<<std::endl;
    assert((((size_t) node) & TOTAL_MASK) == 0);
    node->capacity = degree;
    node->degree = 0;
    node->initSize = 0;
    node->changeSum = 0;
#ifndef IST_DISABLE_MULTICOUNTER_AT_ROOT
    node->externalChangeCounter = NULL;
    assert(!node->externalChangeCounter);
#endif
    node->dirty = 0;
    node->nextMarkAndCount = 0;
    assert(node);
    return node;
}

template <typename K, typename V, class Interpolate, class RecManager>
Node<K,V>* istree<K,V,Interpolate,RecManager>::createLeaf(const int tid, KVPair<K,V> * pairs, int numPairs) {
    auto node = createNode(tid, numPairs+1);
    node->degree = numPairs+1;
    node->initSize = numPairs;
    *node->ptrAddr(0) = EMPTY_VAL_TO_CASWORD;
    for (int i=0;i<numPairs;++i) {
#ifndef NDEBUG
        if (i && pairs[i].k <= pairs[i-1].k) {
            std::cout<<"pairs";
            for (int j=0;j<numPairs;++j) {
                std::cout<<" "<<pairs[j].k;
            }
            std::cout<<std::endl;
        }
#endif
        assert(i==0 || pairs[i].k > pairs[i-1].k);
        node->key(i) = pairs[i].k;
        *node->ptrAddr(i+1) = VAL_TO_CASWORD(pairs[i].v);
    }
    node->minKey = node->key(0);
    node->maxKey = node->key(node->degree-2);
    return node;    
}

template <typename K, typename V, class Interpolate, class RecManager>
Node<K,V>* istree<K,V,Interpolate,RecManager>::createMultiCounterNode(const int tid, const int degree) {
//    GSTATS_ADD(tid, num_multi_counter_node_created, 1);
    auto node = createNode(tid, degree);
#ifndef IST_DISABLE_MULTICOUNTER_AT_ROOT
    node->externalChangeCounter = new MultiCounter(this->NUM_PROCESSES, 1);
//    std::cout<<"created MultiCounter at address "<<node->externalChangeCounter<<std::endl;
    assert(node->externalChangeCounter);
#endif
    return node;
}

template <typename K, typename V, class Interpolate, class RecManager>
KVPair<K,V> * istree<K,V,Interpolate,RecManager>::createKVPair(const int tid, const K& key, const V& value) {
    auto result = new KVPair<K,V>(); //key, value);
    *result = { key, value };
//    std::cout<<"kvpair allocated of size "<<sizeof(KVPair<K,V>)<<" with key="<<key<<" @ "<<(size_t) result<<std::endl;
    assert((((size_t) result) & TOTAL_MASK) == 0);
    assert(result);
    return result;
}

#endif /* BROWN_EXT_IST_LF_IMPL_H */
