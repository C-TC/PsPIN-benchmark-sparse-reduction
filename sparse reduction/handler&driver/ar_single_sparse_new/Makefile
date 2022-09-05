DATA_TYPE = 0
STORE_TYPE = 0
SPARSE_RATIO = 1
hosts = 16
blocks = 16
streams = 1
ALLREDUCE_FLAGS = -DAR_TYPE=${DATA_TYPE} -DSTORAGE_TYPE=${STORE_TYPE} -DBLOCK_TO_NONZERO_RATIO=${SPARSE_RATIO} -DNUM_SWITCH_PORTS=${hosts} -DNUM_BLOCKS=${blocks} -DNUM_STREAMS=$(streams)

SPIN_APP_NAME = ar_single_sparse
SPIN_APP_SRCS = handlers/ar_single_sparse.c
SPIN_CFLAGS = -O3 -g $(ALLREDUCE_FLAGS)
SPIN_LDFLAGS = -lm 

GENERIC_DRIVER_DIR = generic_driver

include $(PSPIN_RT)/rules/spin-handlers.mk
include generic_driver/gdriver.mk

