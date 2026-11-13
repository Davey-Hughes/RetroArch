CC ?= cc
ROOT := ../../..

CORE := save_e2e_core.so
SHIM := stall_shim.so
# Windows lanes: make CC=x86_64-w64-mingw32-gcc save_e2e_core.dll
DLL := save_e2e_core.dll

CFLAGS += -std=gnu89 -Wall -O1 -g -fPIC -I$(ROOT)/libretro-common/include

all: $(CORE) $(SHIM)

$(CORE): save_e2e_core.c
	$(CC) $(CFLAGS) -shared save_e2e_core.c -o $@

$(DLL): save_e2e_core.c
	$(CC) $(CFLAGS) -shared save_e2e_core.c \
	   $(ROOT)/libretro-common/time/rtime.c -o $@

$(SHIM): stall_shim.c
	$(CC) $(CFLAGS) -shared stall_shim.c -o $@ -ldl

clean:
	rm -f $(CORE) $(SHIM) $(DLL)

.PHONY: all clean
