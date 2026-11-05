CC ?= cc
ROOT := ../../..

CORE := save_e2e_core.so
SHIM := stall_shim.so

CFLAGS += -std=gnu89 -Wall -O1 -g -fPIC -I$(ROOT)/libretro-common/include

all: $(CORE) $(SHIM)

$(CORE): save_e2e_core.c
	$(CC) $(CFLAGS) -shared save_e2e_core.c -o $@

$(SHIM): stall_shim.c
	$(CC) $(CFLAGS) -shared stall_shim.c -o $@ -ldl

clean:
	rm -f $(CORE) $(SHIM)

.PHONY: all clean
