#ifndef HOST
#include <handler.h>
#else
#include <handler_profiler.h>
#endif

#include <packets.h>
#include <spin_conf.h>
#include <string.h>
#include "ar_single_sparse.h"

#define NUM_CLUSTERS 4
#define NUM_CORES_PER_CLUSTER 8
#define STRIDE 1
#define OFFSET 0
#define NUM_INT_OP 0

#ifndef HASH_LINEAR_PROBE
    #define HASH_LINEAR_PROBE 0
#endif


#if STORAGE_TYPE == STORAGE_TYPE_DENSE
static  __attribute__((always_inline)) inline void aggregate_block(AllreducePacket* ar, AllreduceInfo* ar_info_local){
    for (uint32_t i = 0; i < ar->hdr.num_values; i++){
        ar_info_local->data[ar->index[i]] += (ar->data)[i];
    }
}

static  __attribute__((always_inline)) inline void flush_block(AllreducePacket* ar, AllreduceInfo* ar_info_local, u_char* out_buffer){
#if DEBUG
    printf("Flushing block id %d\n", ar->hdr.id);
#endif
    int i = 0, j = 0;
    AllreducePacket* ar_out = (AllreducePacket*) (out_buffer + SIZE_IP_UDP_HDRS);
    ar_out->hdr.id = ar->hdr.id;
    ar_out->hdr.num_values = MAX_DATA_ELEMENTS;
    ar_out->hdr.block_split_num = 0;
    uint32_t blocks_sent = 0;
    for(int i = 0; i < BLOCK_RANGE; i++){
        if(ar_info_local->data[i]){
            ar_out->index[j] = i;
            ar_out->data[j] = ar_info_local->data[i];
            ar_info_local->data[i] = 0; // If it was zero no need to set it to zero
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
static  __attribute__((always_inline)) inline void aggregate_block(AllreducePacket* ar, AllreduceInfo* ar_info_local){
    ar_info_local->stash.hdr.id = ar->hdr.id;  
    for (uint32_t i = 0; i < ar->hdr.num_values; i++){
        uint32_t hidx = ar->index[i] % HASH_SIZE;

        #if VALUES_PER_ELEMENT == 1
        //printf("Idx %d data %d index %d\n", hidx, ar_info_local->data[hidx], ar_info_local->index[hidx]);
        if(ar_info_local->data[hidx] == 0){
            ar_info_local->data[hidx] = (ar->data)[i];
            ar_info_local->index[hidx] = ar->index[i];
        }else if(ar_info_local->index[hidx] == ar->index[i]){
            ar_info_local->data[hidx] += (ar->data)[i];
        }
        #if HASH_LINEAR_PROBE == 1
        else if(ar_info_local->data[(hidx + 1) % HASH_SIZE] == 0){
            ar_info_local->data[(hidx + 1) % HASH_SIZE] = (ar->data)[i];
            ar_info_local->index[(hidx + 1) % HASH_SIZE] = ar->index[i];
        } else if(ar_info_local->index[(hidx + 1) % HASH_SIZE] == ar->index[i]){
            ar_info_local->data[(hidx + 1) % HASH_SIZE] += (ar->data)[i];
        }
        #endif
        else{
            // Collision, put it in the output packet
            ar_info_local->stash.index[ar_info_local->stash.hdr.num_values] = (ar->index)[i];
            ar_info_local->stash.data[ar_info_local->stash.hdr.num_values] = (ar->data)[i];
            if(++ar_info_local->stash.hdr.num_values == MAX_DATA_ELEMENTS){
                ar_info_local->stash.hdr.block_split_num = 0;
		        ++ar_info_local->subblocks_out_sent;
                spin_cmd_t handle;
                spin_send_packet(&(ar_info_local->stash), PKT_SIZE, &handle); // Send to the next level of the tree            
                ar_info_local->stash.hdr.num_values = 0;
            }
        }

        #elif VALUES_PER_ELEMENT == 2
        if(ar_info_local->data[2 * hidx] == 0 && ar_info_local->data[2 * hidx + 1] == 0){
            ar_info_local->data[2 * hidx] = (ar->data)[2 * i];
            ar_info_local->data[2 * hidx + 1] = (ar->data)[2 * i  + 1];
            ar_info_local->index[hidx] = ar->index[i];
        }else if(ar_info_local->index[hidx] == ar->index[i]){
            ar_info_local->data[2 * hidx] += (ar->data)[2 * i];
            ar_info_local->data[2 * hidx + 1] += (ar->data)[2 * i  + 1];
        }
        #if HASH_LINEAR_PROBE == 1
        else if(ar_info_local->data[2 * ((hidx + 1) % HASH_SIZE)] == 0 && ar_info_local->data[2 * ((hidx + 1) % HASH_SIZE) + 1] == 0){
            ar_info_local->data[2 * ((hidx + 1) % HASH_SIZE)] = (ar->data)[2 * i];
            ar_info_local->data[2 * ((hidx + 1) % HASH_SIZE) + 1] = (ar->data)[2 * i  + 1];
            ar_info_local->index[((hidx + 1) % HASH_SIZE)] = ar->index[i];
        }else if(ar_info_local->index[((hidx + 1) % HASH_SIZE)] == ar->index[i]){
            ar_info_local->data[2 * ((hidx + 1) % HASH_SIZE)] += (ar->data)[2 * i];
            ar_info_local->data[2 * ((hidx + 1) % HASH_SIZE) + 1] += (ar->data)[2 * i  + 1];
        }
        #endif
        else{
            // Collision, put it in the output packet
            ar_info_local->stash.index[ar_info_local->stash.hdr.num_values] = (ar->index)[i];
            ar_info_local->stash.data[2 * ar_info_local->stash.hdr.num_values] = (ar->data)[2 * i];
            ar_info_local->stash.data[2 * ar_info_local->stash.hdr.num_values + 1] = (ar->data)[2 * i  + 1];
            if(++ar_info_local->stash.hdr.num_values == MAX_DATA_ELEMENTS){
                ar_info_local->stash.hdr.block_split_num = 0;
		        ++ar_info_local->subblocks_out_sent;
                spin_cmd_t handle;
                spin_send_packet(&(ar_info_local->stash), PKT_SIZE, &handle); // Send to the next level of the tree            
                ar_info_local->stash.hdr.num_values = 0;
            }
        }
        #endif
    }
}

static  __attribute__((always_inline)) inline void flush_block(AllreducePacket* ar, AllreduceInfo* ar_info_local, u_char* out_buffer){
#if DEBUG
    printf("Flushing block id %d\n", ar->hdr.id);
#endif
    for(size_t i = 0; i < HASH_SIZE; i++){
        #if VALUES_PER_ELEMENT == 1
        if(ar_info_local->data[i]){
            ar_info_local->stash.index[ar_info_local->stash.hdr.num_values] = ar_info_local->index[i];
            ar_info_local->stash.data[ar_info_local->stash.hdr.num_values] = ar_info_local->data[i];
            ar_info_local->data[i] = 0; // We set it to zero for when the buffer will be reused
            ar_info_local->index[i] = 0; // We set it to zero for when the buffer will be reused
        #elif VALUES_PER_ELEMENT == 2
        if(ar_info_local->data[2 * i] || ar_info_local->data[2 * i + 1]){
            ar_info_local->stash.index[ar_info_local->stash.hdr.num_values] = ar_info_local->index[i];
            ar_info_local->stash.data[2 * ar_info_local->stash.hdr.num_values] = ar_info_local->data[2 * i];
            ar_info_local->stash.data[2 * ar_info_local->stash.hdr.num_values + 1] = ar_info_local->data[2 * i + 1];
            ar_info_local->data[2 * i] = 0; // We set it to zero for when the buffer will be reused
            ar_info_local->data[2 * i + 1] = 0;
            ar_info_local->index[i] = 0; // We set it to zero for when the buffer will be reused
        #endif
            if(++ar_info_local->stash.hdr.num_values == MAX_DATA_ELEMENTS){
                spin_cmd_t handle;
#if DEBUG
                printf("Sending full pkt id %d\n", ar->hdr.id);
#endif            
                ++ar_info_local->subblocks_out_sent;
                spin_send_packet(&(ar_info_local->stash), PKT_SIZE, &handle); // Send to the next level of the tree            
                ar_info_local->stash.hdr.num_values = 0;
            }
        }        
    }
    if(ar_info_local->stash.hdr.num_values){
        spin_cmd_t handle;
#if DEBUG
        printf("Sending pkt with %d elements id %d\n", ar_info_local->stash.hdr.num_values, ar->hdr.id);
#endif            
        ar_info_local->stash.hdr.block_split_num = ++ar_info_local->subblocks_out_sent;       
        spin_send_packet(&(ar_info_local->stash), PKT_SIZE - (AR_TYPE_SIZE*(MAX_DATA_ELEMENTS - ar_info_local->stash.hdr.num_values)), &handle); // Send to the next level of the tree            
        ar_info_local->stash.hdr.num_values = 0;
    }    
    ar_info_local->subblocks_out_sent = 0;
    ar_info_local->num_children = 0;
}
#endif

__handler__ void ar_single_sparse_ph(handler_args_t *args){
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
#if DEBUG
    printf("Trying to lock %p\n", lock);
#endif
    spin_lock_lock(lock);
#if DEBUG
    printf("Locked %p\n", lock);
#endif

    aggregate_block(ar, ar_info_local);
    
    if(ar->hdr.block_split_num){
        ar_info_local->subblocks_in_expected[ar->hdr.port] = ar->hdr.block_split_num;
    }
    if(++ar_info_local->subblocks_in_recvd[ar->hdr.port] == ar_info_local->subblocks_in_expected[ar->hdr.port]){
        ar_info_local->num_children += 1;
        ar_info_local->subblocks_in_recvd[ar->hdr.port] = 0;
        ar_info_local->subblocks_in_expected[ar->hdr.port] = 0;
    }
#if DEBUG
    printf("Num children for id %d: %d\n", ar->hdr.id, ar_info_local->num_children);
#endif

    if(ar_info_local->num_children == NUM_CHILDREN){ // I am the last one
        flush_block(ar, ar_info_local, (u_char*) out_buffer);
    }
    
    spin_lock_unlock(lock);
#if DEBUG
    printf("Unlocked %p\n", lock);
#endif
}

void init_handlers(handler_fn *hh, handler_fn *ph, handler_fn *th, void **handler_mem_ptr)
{
    volatile handler_fn handlers[] = {NULL, ar_single_sparse_ph, NULL};
    *hh = handlers[0];
    *ph = handlers[1];
    *th = handlers[2];

}
