// Copyright 2020 ETH Zurich
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef HOST
#include <handler.h>
#else
#include <handler_profiler.h>
#endif

#include <packets.h>
#include <string.h>
#include <spin_conf.h>
#include "ar_multi_sparse.h"

#define NUM_CLUSTERS 4
#define NUM_CORES_PER_CLUSTER 8
#define STRIDE 1
#define OFFSET 0
#define NUM_INT_OP 0


#if STORAGE_TYPE == STORAGE_TYPE_DENSE
static  __attribute__((always_inline)) inline void aggregate_block(AllreducePacket* ar, AllreduceInfo* ar_info_local, int8_t buffer_id){
    for (uint32_t i = 0; i < ar->hdr.num_values; i++){
        ar_info_local->data[buffer_id][ar->index[i]] += (ar->data)[i];
    }
}

static  __attribute__((always_inline)) inline void flush_block(AllreducePacket* ar, AllreduceInfo* ar_info_local, u_char* out_buffer){
#if DEBUG
    printf("Flushing block id %d\n", ar->hdr.id);
#endif
    // aggregate two buffers
    #if NUM_BUFFERS == 2
        #if USE_SIMD == 1
            #if AR_TYPE == AR_TYPE_INT8
                uint32_t idx;
                for (idx = 0; idx < BLOCK_RANGE/4; idx++){
                    // https://github.com/pulp-platform/pulpino/blob/master/sw/apps/riscv_tests/testVecArith/testVecArith.c
                    asm volatile ("pv.add.h %[c], %[a], %[b]\n" 
                        : [c] "+r" (((uint32_t*) ar_info_local->data[0])[idx])  // Result
                        : [a] "r"  (((uint32_t*) ar_info_local->data[1])[idx]), // First operand
                        [b] "r" (((uint32_t*) ar_info_local->data[0])[idx]));  // Second operand
                }
                for(idx = 4 * idx; idx < BLOCK_RANGE; idx++) {
                    if(ar_info_local->data[1][idx]) {
                        ar_info_local->data[0][idx] += ar_info_local->data[1][idx];
                        ar_info_local->data[1][idx] = 0;
                    }
                }
            #elif AR_TYPE == AR_TYPE_INT16
                uint32_t idx;
                for (idx = 0; idx < BLOCK_RANGE/2; idx++){
                    // https://github.com/pulp-platform/pulpino/blob/master/sw/apps/riscv_tests/testVecArith/testVecArith.c
                    asm volatile ("pv.add.h %[c], %[a], %[b]\n" 
                        : [c] "+r" (((uint32_t*) ar_info_local->data[0])[idx])  // Result
                        : [a] "r"  (((uint32_t*) ar_info_local->data[1])[idx]), // First operand
                        [b] "r" (((uint32_t*) ar_info_local->data[0])[idx]));  // Second operand
                }
                for(idx = 2 * idx; idx < BLOCK_RANGE; idx++) {
                    if(ar_info_local->data[1][idx]) {
                        ar_info_local->data[0][idx] += ar_info_local->data[1][idx];
                        ar_info_local->data[1][idx] = 0;
                    }
                }
            #else 
                #error "NO SIMD on int32/float"
            #endif
        
        #else 
            for(uint32_t i = 0; i < BLOCK_RANGE; i++) {
                if(ar_info_local->data[1][i]) {
                    ar_info_local->data[0][i] += ar_info_local->data[1][i];
                    ar_info_local->data[1][i] = 0;
                }
            }
        #endif
    #else
        #error "Unsupported NUM_BUFFERS"
    #endif

    int i = 0, j = 0;
    AllreducePacket* ar_out = (AllreducePacket*) (out_buffer + SIZE_IP_UDP_HDRS);
    ar_out->hdr.id = ar->hdr.id;
    ar_out->hdr.num_values = MAX_DATA_ELEMENTS;
    ar_out->hdr.block_split_num = 0;
    uint32_t blocks_sent = 0;
    for(int i = 0; i < BLOCK_RANGE; i++){
        if(ar_info_local->data[0][i]){
            ar_out->index[j] = i;
            ar_out->data[j] = ar_info_local->data[0][i];
            ar_info_local->data[0][i] = 0; // If it was zero no need to set it to zero
            if(++j == MAX_DATA_ELEMENTS){
                spin_cmd_t handle;
#if DEBUG
                printf("Sending full pkt id %d\n", ar->hdr.id);
#endif            
                ++blocks_sent;
                spin_send_packet(out_buffer, PKT_SIZE, &handle); // Send to the next level of the tree            
                j = 0;
            }
        }
    }
    if(j){
        ar_out->hdr.num_values = j;
        spin_cmd_t handle;
#if DEBUG
        printf("Sending pkt with %d elements id %d\n", j, ar->hdr.id);
#endif            
        ar_out->hdr.block_split_num = ++blocks_sent;
        spin_send_packet(out_buffer, PKT_SIZE - (AR_TYPE_SIZE*(MAX_DATA_ELEMENTS - j)), &handle); // Send to the next level of the tree            
    }

    ar_info_local->num_children = 0;
}
#elif STORAGE_TYPE == STORAGE_TYPE_HASH
static  __attribute__((always_inline)) inline void aggregate_block(AllreducePacket* ar, AllreduceInfo* ar_info_local, int8_t buffer_id){
    ar_info_local->stash[buffer_id].hdr.id = ar->hdr.id;  
    for (uint32_t i = 0; i < ar->hdr.num_values; i++){
        uint32_t hidx = ar->index[i] % HASH_SIZE;

        //printf("Idx %d data %d index %d\n", hidx, ar_info_local->data[hidx], ar_info_local->index[hidx]);
        if(ar_info_local->data[buffer_id][hidx] == 0){
            ar_info_local->data[buffer_id][hidx] = (ar->data)[i];
            ar_info_local->index[buffer_id][hidx] = ar->index[i];
        }else if(ar_info_local->index[buffer_id][hidx] == ar->index[i]){
            ar_info_local->data[buffer_id][hidx] += (ar->data)[i];
        }else{
            // Collision, put it in the output packet
            ar_info_local->stash[buffer_id].index[ar_info_local->stash[buffer_id].hdr.num_values] = (ar->index)[i];
            ar_info_local->stash[buffer_id].data[ar_info_local->stash[buffer_id].hdr.num_values] = (ar->data)[i];
            if(++ar_info_local->stash[buffer_id].hdr.num_values == MAX_DATA_ELEMENTS){
                ar_info_local->stash[buffer_id].hdr.block_split_num = 0;
                amo_add((uint32_t* )&(ar_info_local->subblocks_out_sent), 1);
                spin_cmd_t handle;
                spin_send_packet(&(ar_info_local->stash[buffer_id]), PKT_SIZE, &handle); // Send to the next level of the tree            
                ar_info_local->stash[buffer_id].hdr.num_values = 0;
            }
        }
    }
}

static  __attribute__((always_inline)) inline void flush_block(AllreducePacket* ar, AllreduceInfo* ar_info_local, u_char* out_buffer){
#if COMPRESSED_SENDING == 0
    #if DEBUG
        printf("Flushing block id %d\n", ar->hdr.id);
    #endif
    // put stash 2 into stash 1
    for(size_t i = 0; i < ar_info_local->stash[1].hdr.num_values; i++){
        if(ar_info_local->data[1][i]){
            ar_info_local->stash[0].index[ar_info_local->stash[0].hdr.num_values] = ar_info_local->stash[1].index[i];
            ar_info_local->stash[0].data[ar_info_local->stash[0].hdr.num_values] = ar_info_local->stash[1].data[i];
            ar_info_local->stash[1].index[i] = 0; // We set it to zero for when the buffer will be reused
            ar_info_local->stash[1].data[i] = 0; // We set it to zero for when the buffer will be reused
            if(++ar_info_local->stash[0].hdr.num_values == MAX_DATA_ELEMENTS){
                spin_cmd_t handle;
#if DEBUG
                printf("Sending full pkt id %d\n", ar->hdr.id);
#endif            
                ++ar_info_local->subblocks_out_sent;
                spin_send_packet(&(ar_info_local->stash[0]), PKT_SIZE, &handle); // Send to the next level of the tree            
                ar_info_local->stash[0].hdr.num_values = 0;
            }
        }        
    }
    ar_info_local->stash[1].hdr.num_values = 0;

    for(size_t buffer_idx = 0; buffer_idx < NUM_BUFFERS; buffer_idx++) {
        for(size_t i = 0; i < HASH_SIZE; i++){
            if(ar_info_local->data[buffer_idx][i]){
                ar_info_local->stash[0].index[ar_info_local->stash[0].hdr.num_values] = ar_info_local->index[buffer_idx][i];
                ar_info_local->stash[0].data[ar_info_local->stash[0].hdr.num_values] = ar_info_local->data[buffer_idx][i];
                ar_info_local->data[buffer_idx][i] = 0; // We set it to zero for when the buffer will be reused
                ar_info_local->index[buffer_idx][i] = 0; // We set it to zero for when the buffer will be reused
                if(++ar_info_local->stash[0].hdr.num_values == MAX_DATA_ELEMENTS){
                    spin_cmd_t handle;
    #if DEBUG
                    printf("Sending full pkt id %d\n", ar->hdr.id);
    #endif            
                    ++ar_info_local->subblocks_out_sent;
                    spin_send_packet(&(ar_info_local->stash[0]), PKT_SIZE, &handle); // Send to the next level of the tree            
                    ar_info_local->stash[0].hdr.num_values = 0;
                }
            }        
        }
    }
    
    if(ar_info_local->stash[0].hdr.num_values){
        spin_cmd_t handle;
#if DEBUG
        printf("Sending pkt with %d elements id %d\n", ar_info_local->stash[0].hdr.num_values, ar->hdr.id);
#endif            
        ar_info_local->stash[0].hdr.block_split_num = ++ar_info_local->subblocks_out_sent;       
        spin_send_packet(&(ar_info_local->stash[0]), PKT_SIZE - (AR_TYPE_SIZE*(MAX_DATA_ELEMENTS - ar_info_local->stash[0].hdr.num_values)), &handle); // Send to the next level of the tree            
        ar_info_local->stash[0].hdr.num_values = 0;
    }    
    ar_info_local->subblocks_out_sent = 0;
    ar_info_local->num_children = 0;
#elif COMPRESSED_SENDING == 1
    //aggregate stash2 to hash table 1
    for(size_t i = 0; i < ar_info_local->stash[1].hdr.num_values; i++){
        uint32_t hidx = ar_info_local->stash[1].index[i] % HASH_SIZE;
        if(ar_info_local->data[0][hidx] == 0){
            ar_info_local->data[0][hidx] = ar_info_local->stash[1].data[i];
            ar_info_local->index[0][hidx] = ar_info_local->stash[1].index[i];
        }else if(ar_info_local->index[0][hidx] == ar_info_local->stash[1].index[i]){
            ar_info_local->data[0][hidx] += ar_info_local->stash[1].data[i];
        }else{
            // Collision, put it in the output packet
            ar_info_local->stash[0].index[ar_info_local->stash[0].hdr.num_values] = ar_info_local->stash[1].index[i];
            ar_info_local->stash[0].data[ar_info_local->stash[0].hdr.num_values] = ar_info_local->stash[1].data[i];
            if(++ar_info_local->stash[0].hdr.num_values == MAX_DATA_ELEMENTS){
                ar_info_local->stash[0].hdr.block_split_num = 0;
                ++ar_info_local->subblocks_out_sent;
                spin_cmd_t handle;
                spin_send_packet(&(ar_info_local->stash[0]), PKT_SIZE, &handle); // Send to the next level of the tree            
                ar_info_local->stash[0].hdr.num_values = 0;
            }
        }
    }
    ar_info_local->stash[1].hdr.num_values = 0;
    //aggregate hash table 2 to hash table 1
    for(size_t i = 0; i < HASH_SIZE; i++) {
        if(ar_info_local->data[1][i]) {
            uint32_t hidx = ar_info_local->index[1][i] % HASH_SIZE;
            if(ar_info_local->data[0][hidx] == 0) {
                ar_info_local->data[0][hidx] = ar_info_local->data[1][i];
                ar_info_local->index[0][hidx] = ar_info_local->index[1][i];
            }else if(ar_info_local->index[0][hidx] == ar_info_local->index[1][i]) {
                ar_info_local->data[0][hidx] = ar_info_local->data[1][i];
            }else {
                // Collision, put it in the output packet
                ar_info_local->stash[0].index[ar_info_local->stash[0].hdr.num_values] = ar_info_local->index[1][i];
                ar_info_local->stash[0].data[ar_info_local->stash[0].hdr.num_values] = ar_info_local->data[1][i];
                if(++ar_info_local->stash[0].hdr.num_values == MAX_DATA_ELEMENTS){
                    ar_info_local->stash[0].hdr.block_split_num = 0;
                    ++ar_info_local->subblocks_out_sent;
                    spin_cmd_t handle;
                    spin_send_packet(&(ar_info_local->stash[0]), PKT_SIZE, &handle); // Send to the next level of the tree            
                    ar_info_local->stash[0].hdr.num_values = 0;
                }
            }
            ar_info_local->data[1][i] = 0;
            ar_info_local->index[1][i] = 0;
        }
    }
    //flush stash1
    for(size_t i = 0; i < HASH_SIZE; i++){
        if(ar_info_local->data[0][i]){
            ar_info_local->stash[0].index[ar_info_local->stash[0].hdr.num_values] = ar_info_local->index[0][i];
            ar_info_local->stash[0].data[ar_info_local->stash[0].hdr.num_values] = ar_info_local->data[0][i];
            ar_info_local->data[0][i] = 0;
            ar_info_local->index[0][i] = 0;
            if(++ar_info_local->stash[0].hdr.num_values == MAX_DATA_ELEMENTS){
                spin_cmd_t handle;
                ++ar_info_local->subblocks_out_sent;
                spin_send_packet(&(ar_info_local->stash[0]), PKT_SIZE, &handle); // Send to the next level of the tree            
                ar_info_local->stash[0].hdr.num_values = 0;
            }
        }        
    }
    if(ar_info_local->stash[0].hdr.num_values){
        spin_cmd_t handle;
        ar_info_local->stash[0].hdr.block_split_num = ++ar_info_local->subblocks_out_sent;       
        spin_send_packet(&(ar_info_local->stash[0]), PKT_SIZE - (AR_TYPE_SIZE*(MAX_DATA_ELEMENTS - ar_info_local->stash[0].hdr.num_values)), &handle); // Send to the next level of the tree            
        ar_info_local->stash[0].hdr.num_values = 0;
    }    
    ar_info_local->subblocks_out_sent = 0;
    ar_info_local->num_children = 0;
#endif
}
#endif

__handler__ void ar_multi_sparse_ph(handler_args_t *args){
#if DEBUG
    printf("Packet handler executed\n");
#endif
    task_t* task = args->task;

    // Packet
    AllreducePacket* ar = (AllreducePacket*) (task->pkt_mem + SIZE_IP_UDP_HDRS);
    // Stored data
    volatile int8_t *local_mem = (int8_t *)(task->scratchpad[args->cluster_id]);
    size_t offset = ((ar->hdr.id / NUM_CLUSTERS) % NUM_MAX_FLYING_PACKETS);
#if DEBUG
    printf("ID %d offset %d localmem %p speroff %d lock %p hpuid %d\n", ar->hdr.id, offset, local_mem, sizeof(uint32_t)*offset, local_mem + sizeof(uint32_t)*offset, args->hpu_id);
#endif
    volatile uint32_t* lock = (uint32_t*) (local_mem + sizeof(uint32_t)*offset);
    // We keep one buffer per core (equal to packet size), for creating the packet to be sent out (would not fit on the stack)
    //                                                 |-------- locks --------------------------|----- out buffers------|
    volatile int8_t* out_buffer = (int8_t*) (local_mem + sizeof(uint32_t)*NUM_MAX_FLYING_PACKETS + PKT_SIZE*(args->hpu_id));
    //                                                         |-------- locks --------------------------|---------- out buffers ---------|------- aggregation data ----|
    AllreduceInfo* ar_info_local = (AllreduceInfo*) (local_mem + sizeof(uint32_t)*NUM_MAX_FLYING_PACKETS + PKT_SIZE*NUM_CORES_PER_CLUSTER + sizeof(AllreduceInfo)*offset);   

    int acquired = 0;
    volatile uint32_t* buffer_lock;

#if AR_TYPE == AR_TYPE_INT32
    int32_t* buffer = NULL;
#elif AR_TYPE == AR_TYPE_INT16
    int16_t* buffer = NULL;
#elif AR_TYPE == AR_TYPE_INT8
    int8_t* buffer = NULL;
#else
    float* buffer = NULL;
#endif

    int8_t buffer_id;
    for(size_t i = 0; i < NUM_BUFFERS; i++){
        buffer_lock = &(ar_info_local->locks[i]);
        acquired = spin_lock_try_lock(buffer_lock);
        if(acquired){
            buffer_id = i;
            buffer = &(ar_info_local->data[i][0]);
            break;
        }
    }
    // Failed to acquire any of the locks
    if(!acquired){
        buffer_id = ar->hdr.rand;
        buffer = &(ar_info_local->data[buffer_id][0]);
        buffer_lock = &(ar_info_local->locks[buffer_id]);
        spin_lock_lock(buffer_lock);
    }
#if DEBUG
    printf("Locked %p\n", buffer_lock);
#endif

    aggregate_block(ar, ar_info_local, buffer_id);

    spin_lock_unlock(buffer_lock);
#if DEBUG
    printf("Unlocked %p\n", buffer_lock);
#endif

    spin_lock_lock(lock);
    
    if(ar->hdr.block_split_num){
        ar_info_local->subblocks_in_expected[ar->hdr.port] = ar->hdr.block_split_num;
    }
    if(++ar_info_local->subblocks_in_recvd[ar->hdr.port] == ar_info_local->subblocks_in_expected[ar->hdr.port]){
        ++ar_info_local->num_children;
        ar_info_local->subblocks_in_recvd[ar->hdr.port] = 0;
        ar_info_local->subblocks_in_expected[ar->hdr.port] = 0;
        if(ar_info_local->num_children == NUM_CHILDREN){ // I am the last one
            flush_block(ar, ar_info_local, (u_char*) out_buffer);
        }
    }
    spin_lock_unlock(lock);
#if DEBUG
    printf("Num children for id %d: %d\n", ar->hdr.id, ar_info_local->num_children);
#endif
    

}

void init_handlers(handler_fn *hh, handler_fn *ph, handler_fn *th, void **handler_mem_ptr)
{
    volatile handler_fn handlers[] = {NULL, ar_multi_sparse_ph, NULL};
    *hh = handlers[0];
    *ph = handlers[1];
    *th = handlers[2];

}
