BIN = rdma-test libvalkey-test
LIBVALKEY_DIR = ../../deps/libvalkey
LIBVALKEY_LIB = $(LIBVALKEY_DIR)/lib/libvalkey.a
LIBVALKEY_RDMA_LIB = $(LIBVALKEY_DIR)/lib/libvalkey_rdma.a
LIBVALKEY_SDS = $(LIBVALKEY_DIR)/src/sds.c

ifeq ($(RDMA_PKGCONFIG),0)
        RDMA_LIBS=$(shell $(PKG_CONFIG) --libs librdmacm libibverbs)
else
        RDMA_LIBS=-lrdmacm -libverbs
endif

all: $(BIN)

rdma-test: rdma-test.c
	@$(CC) $^ -o $@ $(RDMA_LIBS) -lpthread -g
	@echo "\nHint: please check the RDMA environment:"
	@echo "\t~# rdma res show"
	@echo "\n      Then launch valkey-server with RDMA support, Run ./rdma-test to test ..."

$(LIBVALKEY_LIB) $(LIBVALKEY_RDMA_LIB):
	@$(MAKE) -C $(LIBVALKEY_DIR) static USE_RDMA=1

libvalkey-test: libvalkey-test.c $(LIBVALKEY_LIB) $(LIBVALKEY_RDMA_LIB) $(LIBVALKEY_SDS)
	@$(CC) $< $(LIBVALKEY_SDS) -o $@ -I$(LIBVALKEY_DIR)/include -I$(LIBVALKEY_DIR)/include/valkey -I$(LIBVALKEY_DIR)/src $(LIBVALKEY_RDMA_LIB) $(LIBVALKEY_LIB) $(RDMA_LIBS) -lpthread -g

clean:
	rm -rf $(BIN)
