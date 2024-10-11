WASMTIME_VERSION=v25.0.2
WASMTIME_PLATFORM=$(shell sh -c 'uname -s 2>/dev/null | tr A-Z a-z || echo not')
WASMTIME_ARCH=$(shell sh -c 'uname -m 2>/dev/null || echo not')

WASMTIME_PKG_NAME=wasmtime-$(WASMTIME_VERSION)-$(WASMTIME_ARCH)-$(WASMTIME_PLATFORM)-c-api
WASMTIME_PKG_URL=https://github.com/bytecodealliance/wasmtime/releases/download/$(WASMTIME_VERSION)/$(WASMTIME_PKG_NAME).tar.xz

WASTIME_FILE_LIST=lib min include LICENSE README.md

$(WASTIME_FILE_LIST):
	curl -L -O $(WASMTIME_PKG_URL)
	tar -xf $(WASMTIME_PKG_NAME).tar.xz
	mv $(WASMTIME_PKG_NAME)/* .
	rm -r $(WASMTIME_PKG_NAME)
	rm $(WASMTIME_PKG_NAME).tar.xz


clean:
	rm -rf $(WASTIME_FILE_LIST)


.PHONY: clean
