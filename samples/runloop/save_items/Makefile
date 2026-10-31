CC ?= cc
ROOT := ../../..
# The header alone, held to the C89 the C89_BUILD lane uses.
C89_CFLAGS := -std=c89 -pedantic -Werror=pedantic -Wall -Wextra -Werror \
              -Wno-long-long -I$(ROOT)/libretro-common/include
# clang -std=c89 -pedantic flags the _Bool that stdbool.h gives libretro.h.
ifneq ($(findstring clang,$(shell $(CC) --version 2>/dev/null)),)
C89_CFLAGS += -Wno-c99-extensions
endif

all: save_items_header.o

save_items_header.o: save_items_header.c $(ROOT)/libretro-common/include/libretro.h
	$(CC) $(C89_CFLAGS) -c save_items_header.c -o $@

check: save_items_header.o

clean:
	rm -f save_items_header.o

.PHONY: all check clean
