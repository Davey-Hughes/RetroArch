CORE_DIR = ../../..
CC      ?= cc
PYTHON  ?= python3
RETROARCH ?= $(abspath $(CORE_DIR)/retroarch)

ifeq ($(OS),Windows_NT)
   CORE := cloudsync_e2e_libretro.dll
else
   CORE := cloudsync_e2e_libretro.so
endif

CFLAGS += -O1 -g -Wall -fPIC -I$(CORE_DIR)/libretro-common/include

all: $(CORE)

$(CORE): cloudsync_e2e_core.c
	$(CC) $(CFLAGS) -shared -o $@ $< $(LDFLAGS)

check: $(CORE)
	$(PYTHON) -B run.py $(RETROARCH) $(abspath $(CORE)) --group 1 --group 2

clean:
	rm -f cloudsync_e2e_libretro.so cloudsync_e2e_libretro.dll

.PHONY: all check clean
