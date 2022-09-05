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

#include "pspinsim.h"
#include "spin.h"
#include "gdriver_args.h"
#include "gdriver.h"
#include "packets.h"
#include "../handlers/ar_single_sparse.h"
#include "../set/src/set.h"
#include <time.h>

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>

#define SLM_FILES "build/slm_files/"

#define NIC_L2_ADDR 0x1c300000
#define NIC_L2_SIZE (1024 * 1024)

#define HOST_ADDR 0xdeadbeefdeadbeef
#define HOST_SIZE (1024 * 1024 * 1024)

#define SCRATCHPAD_REL_ADDR 0
#define SCRATCHPAD_SIZE (800 * 1024)

#define CHECK_ERR(S)                   \
    {                                  \
        int res;                       \
        if ((res = S) != SPIN_SUCCESS) \
            return res;                \
    }


#ifndef NUM_STREAMS
    #define NUM_STREAMS 1
#endif

typedef struct {
    uint32_t msgid; 
    uint8_t pkt_data[PKT_SIZE];
    size_t pkt_len;
    size_t pkt_l1_len;
    uint8_t eom;
    uint32_t wait_cycles;
    uint64_t user_ptr;
}PacketInfo;

typedef struct {
    uint32_t size;
    PacketInfo pkts[2 * NUM_SWITCH_PORTS * NUM_BLOCKS]
}StreamInfo;

static uint32_t send_time_per_port[NUM_STREAMS][NUM_SWITCH_PORTS][NUM_BLOCKS];
static uint32_t sent[NUM_STREAMS][NUM_BLOCKS];
static uint32_t sent_flag[NUM_STREAMS][NUM_SWITCH_PORTS][NUM_BLOCKS];
static StreamInfo stream[NUM_STREAMS];

int save_packet(size_t stream_id, uint32_t msgid, uint8_t* pkt_data, size_t pkt_len, size_t pkt_l1_len, uint8_t eom, uint32_t wait_cycles, uint64_t user_ptr) {
    stream[stream_id].pkts[stream[stream_id].size].msgid = msgid;
    stream[stream_id].pkts[stream[stream_id].size].pkt_len = pkt_len;
    stream[stream_id].pkts[stream[stream_id].size].pkt_l1_len = pkt_l1_len;
    stream[stream_id].pkts[stream[stream_id].size].eom = eom;
    stream[stream_id].pkts[stream[stream_id].size].wait_cycles = wait_cycles;
    stream[stream_id].pkts[stream[stream_id].size].user_ptr = user_ptr;
    memcpy(stream[stream_id].pkts[stream[stream_id].size].pkt_data, pkt_data, PKT_SIZE);
    ++stream[stream_id].size;
    return 0;
}
//prepare packets for a single stream
int prepare_packets(size_t stream_id) {
    SimpleSet indexes_set; // Set of distinct indexes
    set_init(&indexes_set);
    uint8_t pkt_buffer[PKT_SIZE];
    AllreducePacket* pkt = (AllreducePacket*) (pkt_buffer + SIZE_IP_UDP_HDRS);
    uint32_t now = 0;
    while(1){
        // Find next port from which sth is received
        uint32_t min_time = INT_MAX, min_port = INT_MAX, min_block = INT_MAX;
        for (int j = 0; j < NUM_SWITCH_PORTS; j++){      
            // Still something to send for port j
            uint32_t nb = -1;
            uint32_t closest = INT_MAX;
            // Search next packet to send for this port
            for(int i = 0; i < NUM_BLOCKS; i++){
                if(!sent_flag[stream_id][j][i] && send_time_per_port[stream_id][j][i] >= now && send_time_per_port[stream_id][j][i] < closest){
                    closest = send_time_per_port[stream_id][j][i];
                    nb = i;
                }
            }

            if(nb != -1){
                uint32_t send_time = send_time_per_port[stream_id][j][nb];
                if(send_time < min_time){
                    min_time = send_time;
                    min_port = j;
                    min_block = nb;
                }
            }
        }

        if(min_time == INT_MAX){
            // Nothing else to send
            break;
        }else{
            uint32_t interarrival = min_time - now;
            now = min_time;
            //printf("Sending packet of block %d on port %d after %d ns\n", min_block, min_port, interarrival);
            sent_flag[stream_id][min_port][min_block] = 1;
            pkt->hdr.id = min_block;
            sent[stream_id][min_block]++;
            //printf("Last for block %d %d\n", min_block, sent[min_block]);
            // Set stuff in the packet
#if AR_TYPE == AR_TYPE_INT32
            int32_t tmp_data[BLOCK_RANGE];  
#elif AR_TYPE == AR_TYPE_INT16
            int16_t tmp_data[BLOCK_RANGE];  
#elif AR_TYPE == AR_TYPE_INT8
            int8_t tmp_data[BLOCK_RANGE];  
#elif AR_TYPE == AR_TYPE_FLOAT
            float tmp_data[BLOCK_RANGE];  
#endif
            size_t nonzeros = 0;
            for(size_t i = 0; i < BLOCK_RANGE; i++){
                if((double)rand() / (double)RAND_MAX < 1.0/BLOCK_TO_NONZERO_RATIO){
                    tmp_data[i] = 1;
                    ++nonzeros;
                }else{
                    tmp_data[i] = 0;
                }
            }
            if(nonzeros == 0){ // TODO For the moment we assume there are not full-zero blocks                
                tmp_data[0] = 1;
                ++nonzeros;
            }

            int block_split_num = ceil((float)nonzeros/MAX_DATA_ELEMENTS);
            int chunks_sent = 0;
            //printf("block_split_num for block %d: %d (nonzeros %d)\n", pkt->hdr.id, block_split_num, nonzeros);
            size_t j = 0;
            for(size_t i = 0; i < BLOCK_RANGE; i++){
                if(tmp_data[i]){
                    pkt->index[j] = i;
                    pkt->data[j]= 1;
                    ++j;
                    
                    // Add index to the set
                    char str[16];
                    sprintf(str, "%ld", ((long)BLOCK_RANGE)*pkt->hdr.id + i);
                    set_add(&indexes_set, str);
                    if(j == MAX_DATA_ELEMENTS){
                        pkt->hdr.num_values = MAX_DATA_ELEMENTS;
                        pkt->hdr.port = min_port;
                        pkt->hdr.block_split_num = 0;
                        if(chunks_sent){
                            interarrival = 0;
                        }
                        ++chunks_sent;
                        // (spin_ec_t* ec, uint32_t msgid, uint8_t* pkt_data, size_t pkt_len, size_t pkt_l1_len, uint8_t eom, uint32_t wait_cycles)
                        //pspinsim_packet_add(&ec, pkt->hdr.id, pkt_buffer, PKT_SIZE, PKT_SIZE, sent[min_block] == NUM_SWITCH_PORTS && chunks_sent == block_split_num, interarrival, 0x3);                        
                        save_packet(stream_id, NUM_BLOCKS * stream_id + pkt->hdr.id, pkt_buffer, PKT_SIZE, PKT_SIZE, sent[stream_id][min_block] == NUM_SWITCH_PORTS && chunks_sent == block_split_num, interarrival, 0);
                        j = 0;
                    }
                }else{
                    // It is a zero element, so do nothing
                }
            }
            if(j){
                pkt->hdr.num_values = j;
                pkt->hdr.port = min_port;
                pkt->hdr.block_split_num = block_split_num;
                if(chunks_sent){
                    interarrival = 0;
                }
                ++chunks_sent;
		/*
		int to_remove = (AR_TYPE_SIZE*(MAX_DATA_ELEMENTS - j));
		while((PKT_SIZE - to_remove) % 64){
		  to_remove -= 1;
		}
		assert((PKT_SIZE - to_remove) % 64 == 0);
		*/
		
		int to_remove = 0;
                //printf("Sending only %d elements Removing %d from packet size\n", j, AR_TYPE_SIZE*(MAX_DATA_ELEMENTS - j));
                // (spin_ec_t* ec, uint32_t msgid, uint8_t* pkt_data, size_t pkt_len, size_t pkt_l1_len, uint8_t eom, uint32_t wait_cycles)
                //pspinsim_packet_add(&ec, pkt->hdr.id, pkt_buffer, PKT_SIZE - to_remove, PKT_SIZE - to_remove, sent[min_block] == NUM_SWITCH_PORTS && chunks_sent == block_split_num, interarrival, 0x3);                
                save_packet(stream_id, NUM_BLOCKS * stream_id + pkt->hdr.id, pkt_buffer, PKT_SIZE - to_remove, PKT_SIZE - to_remove, sent[stream_id][min_block] == NUM_SWITCH_PORTS && chunks_sent == block_split_num, interarrival, 0);
                j = 0;
            }
        }
    }
    uint64_t distinct_indexes = set_length(&indexes_set);
    double num_packets = (distinct_indexes/(double)MAX_DATA_ELEMENTS);
    double num_bytes = num_packets*PKT_SIZE;
    printf("Stream %d :Prepare to send %ld distinct indexes. Min num packets %lf Min num bytes %lf\n ", stream_id, distinct_indexes, num_packets, num_bytes);
}

typedef struct sim_descr
{
    const char *handlers_exe;

    //handler names
    const char *hh_name;
    const char *ph_name;
    const char *th_name;

    //packet generator
    uint32_t num_messages;
    uint32_t num_packets;
    uint32_t packet_size;
    uint32_t packet_delay;
    uint32_t message_delay;

    //L2
    uint32_t handler_mem_addr;
    uint32_t handler_mem_size;

    //L1
    uint32_t scratchpad_addr[NUM_CLUSTERS];
    uint32_t scratchpad_size[NUM_CLUSTERS];

    //host memory
    uint64_t host_mem_addr;
    uint64_t host_mem_size;

    spin_ec_t ec;

    fill_packet_fun_t pkt_fill_fun;
    void *l2_img_to_copy;
    size_t l2_img_to_copy_size;

    // counters
    uint32_t packets_sent;
    uint32_t packets_processed;
} sim_descr_t;

static sim_descr_t sim_state;

void pcie_mst_write_complete(void *user_ptr);

int make_ec()
{
    spin_nic_addr_t hh_addr = 0, ph_addr = 0, th_addr = 0;
    size_t hh_size = 0, ph_size = 0, th_size = 0;

    if (sim_state.hh_name != NULL) 
    {
        CHECK_ERR(spin_find_handler_by_name(sim_state.handlers_exe, sim_state.hh_name, &hh_addr, &hh_size));
    }

    if (sim_state.ph_name != NULL) 
    {
        CHECK_ERR(spin_find_handler_by_name(sim_state.handlers_exe, sim_state.ph_name, &ph_addr, &ph_size));
    }

    if (sim_state.th_name != NULL) 
    {
        CHECK_ERR(spin_find_handler_by_name(sim_state.handlers_exe, sim_state.th_name, &th_addr, &th_size));
    }

    printf("hh_addr: %x; hh_size: %lu;\n", hh_addr, hh_size);
    printf("ph_addr: %x; ph_size: %lu;\n", ph_addr, ph_size);
    printf("th_addr: %x; th_size: %lu;\n", th_addr, th_size);

    sim_state.ec.handler_mem_addr = sim_state.handler_mem_addr;
    sim_state.ec.handler_mem_size = sim_state.handler_mem_size;
    sim_state.ec.host_mem_addr = sim_state.host_mem_addr;
    sim_state.ec.host_mem_size = sim_state.host_mem_size;
    sim_state.ec.hh_addr = hh_addr;
    sim_state.ec.ph_addr = ph_addr;
    sim_state.ec.th_addr = th_addr;
    sim_state.ec.hh_size = hh_size;
    sim_state.ec.ph_size = ph_size;
    sim_state.ec.th_size = th_size;

    for (int i = 0; i < NUM_CLUSTERS; i++)
    {
        sim_state.ec.scratchpad_addr[i] = sim_state.scratchpad_addr[i];
        sim_state.ec.scratchpad_size[i] = sim_state.scratchpad_size[i];
    }
}


void generate_packets()
{
    /* spin_ec_t ec;

    uint8_t *pkt_buff = (uint8_t *)malloc(sizeof(uint8_t) * (sim_state.packet_size));
    assert(pkt_buff != NULL);

    for (uint32_t msg_idx = 0; msg_idx < sim_state.num_messages; msg_idx++)
    {
        for (uint32_t pkt_idx = 0; pkt_idx < sim_state.num_packets; pkt_idx++)
        {
            uint32_t pkt_size = sim_state.packet_size;
            uint32_t l1_pkt_size = pkt_size;

            if (sim_state.pkt_fill_fun != NULL)
            {
                pkt_size = sim_state.pkt_fill_fun(msg_idx, pkt_idx, pkt_buff, sim_state.packet_size, &l1_pkt_size);
            }
            else
            {
                // generate IP+UDP headers
                pkt_hdr_t *hdr = (pkt_hdr_t*) pkt_buff;
                hdr->ip_hdr.ihl = 5;
                hdr->ip_hdr.length = pkt_size;
                // TODO: set other fields
            }

            bool is_last = (pkt_idx + 1 == sim_state.num_packets);
            uint32_t delay = (is_last) ? sim_state.message_delay : sim_state.packet_delay;
            pspinsim_packet_add(&(sim_state.ec), msg_idx, pkt_buff, pkt_size, l1_pkt_size, is_last, delay, 0);
            sim_state.packets_sent++;
        }
    } */

    for(int i = 0; i < NUM_STREAMS; i++) {
        prepare_packets(i);
    }
    //pick a stream to send
    int32_t next_stream = 0;
    uint32_t global_time = 0;
    uint32_t stream_time[NUM_STREAMS], packet_counter[NUM_STREAMS];
    for(int i = 0; i < NUM_STREAMS; i++) {
        stream_time[i] = 0;
        packet_counter[i] = 0;
        if(stream[i].pkts->wait_cycles < stream[next_stream].pkts->wait_cycles) {
            next_stream = i;
        }
    }
    while(1) {
        PacketInfo* next_pkt = &(stream[next_stream].pkts[packet_counter[next_stream]]);
        uint32_t delay = next_pkt->wait_cycles + stream_time[next_stream] - global_time;
        printf("Stream %d: sending packet %d, delay %d, msgid %d, pkt_len %d, pkt_l1_len %d, eom %d \n", next_stream, packet_counter[next_stream], delay, next_pkt->msgid, next_pkt->pkt_len, next_pkt->pkt_l1_len, next_pkt->eom);
        pspinsim_packet_add(&(sim_state.ec), next_pkt->msgid, next_pkt->pkt_data, next_pkt->pkt_len, next_pkt->pkt_l1_len, next_pkt->eom, delay, next_pkt->user_ptr);
        stream_time[next_stream] += next_pkt->wait_cycles;
        global_time = stream_time[next_stream];
        printf("Global time: %d\n", global_time);
        ++packet_counter[next_stream];
        next_stream = -1;

        //select next stream
        uint32_t min_time = UINT_MAX;
        for(int32_t i = 0; i < NUM_STREAMS; i++) {
            if(packet_counter[i] >= stream[i].size) continue;
            if(stream[i].pkts[packet_counter[i]].wait_cycles + stream_time[i] < min_time) {
                next_stream = i;
                min_time = stream[i].pkts[packet_counter[i]].wait_cycles + stream_time[i];
            }
        }
        if(next_stream < 0) {
            //no more packets to send
            break;
        }
    }

    pspinsim_packet_eos();

    //free(pkt_buff);
}

void pcie_mst_write_complete(void *user_ptr)
{
    printf("Write to NIC memory completed (user_ptr: %p)\n", user_ptr);
    printf("generating packets in pcie_mst_write_complete\n");
    generate_packets();
}

void feedback(uint64_t user_ptr, uint64_t nic_arrival_time, uint64_t pspin_arrival_time, uint64_t feedback_time)
{
    sim_state.packets_processed++;
}

/*** interface ***/

int gdriver_set_packet_fill_callback(fill_packet_fun_t pkt_fill_fun)
{
    sim_state.pkt_fill_fun = pkt_fill_fun;
    return GDRIVER_OK;
}

int gdriver_set_l2_img(void *img, size_t size)
{
    sim_state.l2_img_to_copy = img;
    sim_state.l2_img_to_copy_size = size;
    return GDRIVER_OK;
}

// Returns an exponentially distributed random number with an expected given mean
static double ran_expo(double mean){
    double lambda = 1.0 / mean;
    double u;
    u = rand() / (RAND_MAX + 1.0);
    return -log(1- u) / lambda;
}

int gdriver_init(int argc, char **argv, const char *hfile, const char *hh, const char *ph, const char *th)
{
    printf("BLOCK RANGE %d\n", BLOCK_RANGE);
    srand(time(NULL));
    uint32_t mean_host_gbps = 400.0/NUM_SWITCH_PORTS/NUM_STREAMS;
    uint32_t mean_host_interdeparture = (PKT_SIZE*8) / mean_host_gbps;
    for(size_t stream_idx = 0; stream_idx < NUM_STREAMS; stream_idx++) {
        for(uint32_t i = 0; i < NUM_SWITCH_PORTS; i++){
            uint32_t send_time = 0;
            uint32_t start_index = 0;
    #if STAGGERED_SENDING
            double num_trains = NUM_BLOCKS/NUM_SWITCH_PORTS;
            start_index = i*num_trains;
    #endif
            uint32_t k = 0;
            for(uint32_t j = start_index; k < NUM_BLOCKS; j = (j+1) % NUM_BLOCKS){
                sent[stream_idx][j] = 0;
                sent_flag[stream_idx][i][j] = 0;
                send_time_per_port[stream_idx][i][j] = send_time + ran_expo(mean_host_interdeparture);
                send_time = send_time_per_port[stream_idx][i][j];
                //printf("I'll send packet of block %d on port %d at %d \n", j, i, send_time_per_port[i][j]);            
                ++k;
            }
        }    
    }   


    struct gengetopt_args_info ai;
    pspin_conf_t conf;

    if (cmdline_parser(argc, argv, &ai) != 0)
    {
        return GDRIVER_ERR;
    }

    pspinsim_default_conf(&conf);
    conf.slm_files_path = SLM_FILES;

    pspinsim_init(argc, argv, &conf);

    pspinsim_cb_set_pcie_mst_write_completion(pcie_mst_write_complete);
    pspinsim_cb_set_pkt_feedback(feedback);

    memset(&sim_state, 0, sizeof(sim_state));

    sim_state.handlers_exe = hfile;
    sim_state.hh_name = hh;
    sim_state.ph_name = ph;
    sim_state.th_name = th;
    sim_state.num_messages = ai.num_messages_arg;
    sim_state.num_packets = ai.num_packets_arg;
    sim_state.packet_size = ai.packet_size_arg;
    sim_state.packet_delay = ai.packet_delay_arg;
    sim_state.message_delay = ai.message_delay_arg;
    sim_state.handler_mem_addr = NIC_L2_ADDR;
    sim_state.handler_mem_size = NIC_L2_SIZE;
    sim_state.host_mem_addr = HOST_ADDR;
    sim_state.host_mem_size = HOST_SIZE;

    sim_state.scratchpad_addr[NUM_CLUSTERS];
    sim_state.scratchpad_size[NUM_CLUSTERS];

    for (int i = 0; i < NUM_CLUSTERS; i++)
    {
        sim_state.scratchpad_addr[i] = SCRATCHPAD_REL_ADDR;
        sim_state.scratchpad_size[i] = SCRATCHPAD_SIZE;
    }

    make_ec();

    return GDRIVER_OK;
}

int gdriver_run()
{
    if (sim_state.l2_img_to_copy != NULL)
    {
        spin_nic_addr_t dest = sim_state.handler_mem_addr;
        uint32_t dest_capacity = sim_state.handler_mem_size;
        void *src = sim_state.l2_img_to_copy;
        spin_nic_addr_t src_size = sim_state.l2_img_to_copy_size;

        assert(src_size <= dest_capacity);
        printf("Copying %d bytes to %x (src: %p)\n", src_size, dest, src);
        spin_nicmem_write(dest, (void *)src, src_size, (void *)0);
    }
    else
    {
        printf("generating packets in gdriver_run\n");
        generate_packets();
    }

    if (pspinsim_run() == SPIN_SUCCESS)
        return GDRIVER_OK;
    else
        return GDRIVER_ERR;
}

int gdriver_fini()
{
    if (pspinsim_fini() == SPIN_SUCCESS)
        return sim_state.packets_sent == sim_state.packets_processed;
    return GDRIVER_ERR;
}
