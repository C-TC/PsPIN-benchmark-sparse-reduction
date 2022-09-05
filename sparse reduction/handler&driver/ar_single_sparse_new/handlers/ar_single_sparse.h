#ifndef NUM_SWITCH_PORTS
#define NUM_SWITCH_PORTS 16
#endif

#define NUM_CHILDREN NUM_SWITCH_PORTS

#ifndef NUM_BLOCKS
#define NUM_BLOCKS 32
#endif 

#define NUM_MAX_FLYING_PACKETS (NUM_BLOCKS)
#undef PKT_SIZE
#define PKT_SIZE 1024
#define STAGGERED_SENDING 1

#define SIZE_IP_UDP_HDRS 28 // We assume no IP options

#define STORAGE_TYPE_DENSE 0
#define STORAGE_TYPE_HASH 1
#define STORAGE_TYPE_LIST 2

#ifndef STORAGE_TYPE
#define STORAGE_TYPE STORAGE_TYPE_DENSE
#endif

#define AR_TYPE_INT32 0
#define AR_TYPE_INT16 1
#define AR_TYPE_INT8 2
#define AR_TYPE_FLOAT 3

#ifndef AR_TYPE
#define AR_TYPE AR_TYPE_INT32
#endif 

#ifndef VALUES_PER_ELEMENT
    #define VALUES_PER_ELEMENT 1
#endif

#if STORAGE_TYPE == STORAGE_TYPE_HASH
    #warning "USING HASH TABLE"
#endif 

#if VALUES_PER_ELEMENT == 1
    // We add  + sizeof(uint16_t) because we have to send the index. Index will be relative to the block
    #if AR_TYPE == AR_TYPE_INT32
        #define AR_TYPE_NAME int32_t
        #define HASH_SIZE 256 //Power of 2 for faster modulo (MAX_DATA_ELEMENTS*1)
        #define SLACK 16
    #elif AR_TYPE == AR_TYPE_INT16
        #if USE_AMO == 0
            #define AR_TYPE_NAME int16_t
            #define HASH_SIZE 512 //Power of 2 for faster modulo (MAX_DATA_ELEMENTS*1)
            #define SLACK 32
        #else
            #error "USE_AMO must be set to 0 when using AR_TYPE_INT16"
        #endif
    #elif AR_TYPE == AR_TYPE_INT8
        #if USE_AMO == 0
            #define AR_TYPE_NAME int8_t
            #define HASH_SIZE 1024 //Power of 2 for faster modulo (MAX_DATA_ELEMENTS*1)
            #define SLACK 64
        #else
            #error "USE_AMO must be set to 0 when using AR_TYPE_INT8"
        #endif
    #elif AR_TYPE == AR_TYPE_FLOAT
        #if USE_AMO == 0
            #define AR_TYPE_NAME float        
            #define HASH_SIZE 256 //Power of 2 for faster modulo (MAX_DATA_ELEMENTS*1)
            #define SLACK 16
        #else
            #error "USE_AMO must be set to 0 when using AR_TYPE_FLOAT"
        #endif
    #else
        #error "Unsupported type"
    #endif

#elif VALUES_PER_ELEMENT == 2
    // We add  + sizeof(uint16_t) because we have to send the index. Index will be relative to the block
    #if AR_TYPE == AR_TYPE_INT32
        #define AR_TYPE_NAME int32_t
        #define HASH_SIZE 128 //Power of 2 for faster modulo (MAX_DATA_ELEMENTS*1)
        #define SLACK 8
    #elif AR_TYPE == AR_TYPE_INT16
        #if USE_AMO == 0
            #define AR_TYPE_NAME int16_t
            #define HASH_SIZE 256 //Power of 2 for faster modulo (MAX_DATA_ELEMENTS*1)
            #define SLACK 16
        #else
            #error "USE_AMO must be set to 0 when using AR_TYPE_INT16"
        #endif
    #elif AR_TYPE == AR_TYPE_INT8
        #if USE_AMO == 0
            #define AR_TYPE_NAME int8_t
            #define HASH_SIZE 512 //Power of 2 for faster modulo (MAX_DATA_ELEMENTS*1)
            #define SLACK 32
        #else
            #error "USE_AMO must be set to 0 when using AR_TYPE_INT8"
        #endif
    #elif AR_TYPE == AR_TYPE_FLOAT
        #if USE_AMO == 0
            #define AR_TYPE_NAME float        
            #define HASH_SIZE 128 //Power of 2 for faster modulo (MAX_DATA_ELEMENTS*1)
            #define SLACK 8
        #else
            #error "USE_AMO must be set to 0 when using AR_TYPE_FLOAT"
        #endif
    #else
        #error "Unsupported type"
    #endif
#else
    #error "Unsupported VALUES_PER_ELEMENT"
#endif

#define AR_TYPE_SIZE (VALUES_PER_ELEMENT * sizeof(AR_TYPE_NAME) + sizeof(uint16_t))

typedef struct{
    uint32_t id; // block id
    uint32_t root_address;
    uint16_t num_values; // Number of values set, MORE PRECISELY, NUM_ELEMENTS, WE CAN HAVE SEVERAL VALUES IN AN ELEMENT
    uint8_t block_split_num; // In how many packets the block has been split. If 0, we don't know it yet
    uint8_t port;
}AllreduceHeader; // TODO: What if size non-multiple of 4 and so the data is not 4-bytes aligned?

#define MAX_DATA_ELEMENTS ((PKT_SIZE - SIZE_IP_UDP_HDRS - sizeof(AllreduceHeader)) / AR_TYPE_SIZE)

#ifndef BLOCK_TO_NONZERO_RATIO
#define BLOCK_TO_NONZERO_RATIO 100 // 1 nonzero element every BLOCK_TO_NONZERO_RATIO elements
#endif
#define BLOCK_RANGE ((MAX_DATA_ELEMENTS-SLACK)*BLOCK_TO_NONZERO_RATIO * VALUES_PER_ELEMENT)

typedef struct{
    AllreduceHeader hdr;
    uint16_t index[MAX_DATA_ELEMENTS];
#if AR_TYPE == AR_TYPE_INT32
    int32_t data[MAX_DATA_ELEMENTS*VALUES_PER_ELEMENT];  
#elif AR_TYPE == AR_TYPE_INT16
    int16_t data[MAX_DATA_ELEMENTS*VALUES_PER_ELEMENT];  
#elif AR_TYPE == AR_TYPE_INT8
    int8_t data[MAX_DATA_ELEMENTS*VALUES_PER_ELEMENT];  
#elif AR_TYPE == AR_TYPE_FLOAT
    float data[MAX_DATA_ELEMENTS*VALUES_PER_ELEMENT];  
#endif
}AllreducePacket;

typedef struct{
    int32_t num_children;
    uint8_t subblocks_in_expected[NUM_SWITCH_PORTS]; // In how many packets the block has been split
    uint8_t subblocks_in_recvd[NUM_SWITCH_PORTS]; // In how many packets the block has been split    
#if STORAGE_TYPE == STORAGE_TYPE_DENSE
    #if AR_TYPE == AR_TYPE_INT32
        int32_t data[BLOCK_RANGE];  
    #elif AR_TYPE == AR_TYPE_INT16
        int16_t data[BLOCK_RANGE];
    #elif AR_TYPE == AR_TYPE_INT8
        int8_t data[BLOCK_RANGE];
    #elif AR_TYPE == AR_TYPE_FLOAT
        float data[BLOCK_RANGE];  
    #endif
#elif STORAGE_TYPE == STORAGE_TYPE_HASH
    uint8_t subblocks_out_sent; // In how many packets the block has been split
    AllreducePacket stash;
    uint16_t index[HASH_SIZE];
    #if AR_TYPE == AR_TYPE_INT32
        int32_t data[HASH_SIZE*VALUES_PER_ELEMENT];  
    #elif AR_TYPE == AR_TYPE_INT16
        int16_t data[HASH_SIZE*VALUES_PER_ELEMENT];
    #elif AR_TYPE == AR_TYPE_INT8
        int8_t data[HASH_SIZE*VALUES_PER_ELEMENT];
    #elif AR_TYPE == AR_TYPE_FLOAT
        float data[HASH_SIZE*VALUES_PER_ELEMENT];  
    #endif
#endif
}AllreduceInfo;
