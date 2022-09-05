#!/bin/bash

#target=$(cat transcript | tail -20 | head -12 | grep -Eo '[0-9\.]+' | tr "\n" " ")
#echo "front " $target  >> result.csv
DATATYPES=("int32" "int16" "int8" "float")
STORAGETYPES=("array" "hash")
echo "Hosts Blocks Datatype Solution Storage Sparsity Streams InPkts InBytes InAvgPktLen FeedbackThroughput FeedbackArrival PktAvgLat PktMinLat PktMaxLat HERstalls Commands OutPkts OutBytes OutAvgPktLen PktThroughput PktDepTime" > result.csv


for dtype in 1 2 3; do
    for sparse in 2 8; do
        for storage in 0 1; do
            make deploy driver -j ALLREDUCE_FLAGS="-DAR_TYPE=${dtype} -DSTORAGE_TYPE=${storage} -DBLOCK_TO_NONZERO_RATIO=${sparse} -DNUM_SWITCH_PORTS=16 -DNUM_BLOCKS=32 -DNUM_STREAMS=1"
            echo 16 32 ${DATATYPES[${dtype}]} "ar_single_sparse" ${STORAGETYPES[${storage}]} $sparse 1
            ./sim_ar_single_sparse > transcript
            target=$(cat transcript | tail -20 | head -12 | grep -Eo '[0-9\.]+' | tr "\n" " ")
            echo 16 32 ${DATATYPES[${dtype}]} "ar_single_sparse" ${STORAGETYPES[${storage}]} $sparse 1 $target  >> result.csv
        done
    done
done



cd ../ar_multi_sparse_new
./test3.sh