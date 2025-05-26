package contention.benchmark.workload.thread.loops.abstractions;

import java.lang.reflect.Method;
import java.util.Collection;

import contention.abstractions.CompositionalMap;
import contention.abstractions.DataStructure;
import contention.benchmark.statistic.ThreadStatistic;
import contention.benchmark.workload.stop.condition.StopCondition;

public abstract class ThreadLoop implements Runnable {
    protected StopCondition stopCondition;

    protected final DataStructure<Integer> dataStructure;

    /**
     * The number of the current thread
     */
    protected final int threadId;
    /**
     * The pool of methods that can run
     */
    protected Method[] methods;
    /**
     * The stop flag, indicating whether the loop is over
     */
    protected volatile boolean stop = false;
    public ThreadStatistic stats = new ThreadStatistic();

    protected ThreadLoop(int threadId, DataStructure<Integer> dataStructure,
                                 Method[] methods, StopCondition stopCondition) {
        this.threadId = threadId;
        this.methods = methods;
        this.dataStructure = dataStructure;
        this.stopCondition = stopCondition;
    }

    public abstract void step();

    public void stopThread() {
        stop = true;
    }

    public void printDataStructure() {
        System.out.println(dataStructure.toString());
    }

    public void run() {
        while (!stopCondition.isStopped(threadId)) {
            step();
            stats.total++;

            assert stats.total == stats.failures + stats.numContains + stats.numSize + stats.numRemove
                    + stats.numAdd + stats.numRemoveAll + stats.numAddAll;
        }
        // System.out.println(numAdd + " " + numRemove + " " + failures);
        this.stats.getCount = CompositionalMap.counts.get().getCount;
        this.stats.nodesTraversed = CompositionalMap.counts.get().nodesTraversed;
        this.stats.insertNodesTraversed = CompositionalMap.counts.get().insertNodesTraversed;
        this.stats.deleteNodesTraversed = CompositionalMap.counts.get().deleteNodesTraversed;
        this.stats.structMods = CompositionalMap.counts.get().structMods;
        this.stats.foundCnt = CompositionalMap.counts.get().foundCnt;
        this.stats.foundTreeTraversed = CompositionalMap.counts.get().foundTreeTraversed;
        this.stats.foundLogicalTraversed = CompositionalMap.counts.get().foundLogicalTraversed;
        this.stats.notFoundCnt = CompositionalMap.counts.get().notFoundCnt;
        this.stats.notFoundTreeTraversed = CompositionalMap.counts.get().notFoundTreeTraversed;
        this.stats.notFoundLogicalTraversed = CompositionalMap.counts.get().notFoundLogicalTraversed;
        this.stats.failedLockAcquire = CompositionalMap.counts.get().failedLockAcquire;
        System.out.println("Thread #" + threadId + " finished.");
    }

    public Integer executeInsert(int key) {
        Integer result;
        if ((result = dataStructure.insert(key)) == null) {
            stats.numAdd++;
        } else {
            stats.failures++;
        }
        return result;
    }

    public Integer executeRemove(int key) {
        Integer result;
        if ((result = dataStructure.remove(key)) != null) {
            stats.numRemove++;
        } else {
            stats.failures++;
        }
        return result;
    }

    public Integer executeGet(int key) {
        Integer result;
        if ((result = dataStructure.get(key)) != null) {
            stats.numContains++;
        } else {
            stats.failures++;
        }
        return result;
    }

    public boolean executeRemoveAll(Collection<Integer> c) {
        boolean result = false;
        try {
            result = dataStructure.removeAll(c);
        } catch (Exception e) {
            System.err.println("Unsupported writeAll operations! Leave the default value of the numWriteAlls parameter (0).");
        }

        if (result) {
            stats.numRemoveAll++;
        } else {
            stats.failures++;
        }
        return result;
    }

    public int executeSize() {
        stats.numSize++;
        return dataStructure.size();
    }

}
