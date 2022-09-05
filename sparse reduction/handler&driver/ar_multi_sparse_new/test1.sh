#!/bin/bash

#target=$(cat transcript | tail -20 | head -12 | grep -Eo '[0-9\.]+' | tr "\n" " ")
#echo "front " $target  >> result.csv

echo "Hosts Blocks Datatype Solution Storage Sparsity InPkts InBytes InAvgPktLen FeedbackThroughput FeedbackArrival PktAvgLat PktMinLat PktMaxLat HERstalls Commands OutPkts OutBytes OutAvgPktLen PktThroughput PktDepTime" > result.csv
#array
for hosts in 8 16 32; do
    for blocks in 8 16 32; do
        for sparse in 1 2 8; do
            make deploy driver -j ALLREDUCE_FLAGS="-DAR_TYPE=0 -DSTORAGE_TYPE=0 -DBLOCK_TO_NONZERO_RATIO=${sparse} -DNUM_SWITCH_PORTS=${hosts} -DNUM_BLOCKS=${blocks} -DNUM_STREAMS=1"
            echo $hosts $blocks "int32 ar_multi_sparse array" $sparse
            ./sim_ar_multi_sparse > transcript
            target=$(cat transcript | tail -20 | head -12 | grep -Eo '[0-9\.]+' | tr "\n" " ")
            echo $hosts $blocks "int32 ar_multi_sparse array" $sparse $target  >> result.csv
            
        done
    done
done

#hash
for hosts in 8 16 32; do
    for blocks in 8 16 32; do
        for sparse in 1 2 8 32 128; do
            make deploy driver -j ALLREDUCE_FLAGS="-DAR_TYPE=0 -DSTORAGE_TYPE=1 -DBLOCK_TO_NONZERO_RATIO=${sparse} -DNUM_SWITCH_PORTS=${hosts} -DNUM_BLOCKS=${blocks} -DNUM_STREAMS=1"
            echo $hosts $blocks "int32 ar_multi_sparse hash" $sparse
            ./sim_ar_multi_sparse > transcript
            target=$(cat transcript | tail -20 | head -12 | grep -Eo '[0-9\.]+' | tr "\n" " ")
            echo $hosts $blocks "int32 ar_multi_sparse hash" $sparse $target  >> result.csv
            
        done
    done
done