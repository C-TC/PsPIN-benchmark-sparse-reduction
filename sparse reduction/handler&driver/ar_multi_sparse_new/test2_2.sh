#!/bin/bash

STORAGETYPES=("array" "hash")
echo "Hosts Blocks Datatype Solution Storage Sparsity Streams InPkts InBytes InAvgPktLen FeedbackThroughput FeedbackArrival PktAvgLat PktMinLat PktMaxLat HERstalls Commands OutPkts OutBytes OutAvgPktLen PktThroughput PktDepTime" > result.csv

for hosts in 32; do
    for blocks in 8; do
        for sparse in 8; do
            for streams in 1 2 3 4; do
                for storage in 0 1; do
                    make deploy driver -j ALLREDUCE_FLAGS="-DAR_TYPE=0 -DSTORAGE_TYPE=${storage} -DBLOCK_TO_NONZERO_RATIO=${sparse} -DNUM_SWITCH_PORTS=${hosts} -DNUM_BLOCKS=${blocks} -DNUM_STREAMS=${streams}"
                    echo $hosts $blocks "int32 ar_multi_sparse array" $sparse $streams
                    ./sim_ar_multi_sparse > transcript
                    target=$(cat transcript | tail -20 | head -12 | grep -Eo '[0-9\.]+' | tr "\n" " ")
                    echo $hosts $blocks "int32 ar_multi_sparse" ${STORAGETYPES[${storage}]} $sparse $streams $target  >> result.csv
                done
            done
        done
    done
done