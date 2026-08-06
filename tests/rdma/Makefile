BIN = rdma-test

ifeq ($(RDMA_PKGCONFIG),0)
        RDMA_LIBS=$(shell $(PKG_CONFIG) --libs librdmacm libibverbs)
else
        RDMA_LIBS=-lrdmacm -libverbs
endif

$(BIN): rdma-test.c
	@$(CC) $^ -o $@ $(RDMA_LIBS) -lpthread -g
	@echo "\nHint: please check the RDMA environment:"
	@echo "\t~# rdma res show"
	@echo "\n      Then launch valkey-server with RDMA support, Run ./"$(BIN) "to test ..."

clean:
	rm -rf $(BIN)
