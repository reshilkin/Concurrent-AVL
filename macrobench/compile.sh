#!/bin/bash
# 
# File:   compile.sh
# Author: trbot
#
# Created on May 28, 2017, 9:56:43 PM
#

workloads="TPCC YCSB"

## format is the following is
## data_structure_name:compilation_arguments
algs=( \
    "hash_chaining:-DIDX_HASH=1" \
    "bronson_pext_bst_occ:" \
    "brown_ext_abtree_lf:-DUSE_RANGE_QUERIES -DRQ_UNSAFE" \
    "brown_ext_bslack_lf:-DUSE_RANGE_QUERIES -DRQ_UNSAFE" \
    "brown_ext_bst_lf:-DUSE_RANGE_QUERIES -DRQ_UNSAFE" \
    "natarajan_ext_bst_lf:-DUSE_RANGE_QUERIES -DRQ_UNSAFE" \
)

make_workload_dict() {
    # compile the given workload and algorithm
    fname=compiling.out
    workload=$1
    name=`echo $2 | cut -d":" -f1`
    opts=`echo $2 | cut -d":" -f2-`
    #echo "arg1=$1 arg2=$2 workload=$workload name=$name opts=$opts"
    make -j clean workload="$workload" data_structure_name="$name" data_structure_opts="$opts"
    make -j workload="$workload" data_structure_name="$name" data_structure_opts="$opts" > $fname 2>&1
    if [ $? -ne 0 ]; then
        echo "Compilation FAILED for $workload $name $opts"
    else
        echo "Compiled $workload $name $opts"
        rm -f $fname
    fi
}
export -f make_workload_dict

rm -f compiling*.out

# check for gnu parallel
command -v parallel > /dev/null 2>&1
if [ "$?" -eq "0" ]; then
	parallel make_workload_dict ::: $workloads ::: "${algs[@]}"
else
	for workload in $workloads; do
	for alg in ${algs[@]}; do
		make_workload_dict "$workload" "$alg"
	done
	done
fi

errorfiles=`ls compiling*.out 2> /dev/null`
numerrorfiles=`ls compiling*.out 2> /dev/null | wc -l`
if [ "$numerrorfiles" -ne "0" ]; then
    cat compiling*.out
    echo "ERROR: some compilation command(s) failed. See the following file(s)."
    for x in $errorfiles ; do echo $x ; done
else
    echo "Compilation successful."
fi
