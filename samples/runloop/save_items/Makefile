CC ?= cc
ROOT := ../../..
TARGET := save_items_test
EXE ?=
SOURCES := save_items_test.c $(ROOT)/save_items.c \
           $(ROOT)/libretro-common/file/file_path.c \
           $(ROOT)/libretro-common/lists/string_list.c \
           $(ROOT)/libretro-common/compat/compat_strl.c \
           $(ROOT)/libretro-common/compat/compat_posix_string.c \
           $(ROOT)/libretro-common/encodings/encoding_utf.c \
           $(ROOT)/libretro-common/time/rtime.c
CFLAGS += -std=gnu89 -Wall -g -O1 \
          -I$(ROOT) -I$(ROOT)/libretro-common/include
# The header alone, held to the C89 the C89_BUILD lane uses.
C89_CFLAGS := -std=c89 -pedantic -Werror=pedantic -Wall -Wextra -Werror \
              -Wno-long-long -I$(ROOT)/libretro-common/include
# clang -std=c89 -pedantic flags the _Bool that stdbool.h gives libretro.h.
ifneq ($(findstring clang,$(shell $(CC) --version 2>/dev/null)),)
C89_CFLAGS += -Wno-c99-extensions
endif

ifneq ($(SANITIZER),)
CFLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
LDFLAGS += -fsanitize=$(SANITIZER)
endif

all: $(TARGET)$(EXE)

save_items_header.o: save_items_header.c $(ROOT)/libretro-common/include/libretro.h
	$(CC) $(C89_CFLAGS) -c save_items_header.c -o $@

$(TARGET)$(EXE): $(SOURCES) save_items_header.o $(ROOT)/save_items.h \
                 $(ROOT)/libretro-common/include/libretro.h
	$(CC) $(CFLAGS) $(SOURCES) save_items_header.o $(LDFLAGS) -o $@

check: $(TARGET)$(EXE)
	./$(TARGET)$(EXE)

clean:
	rm -f $(TARGET) $(TARGET).exe $(TARGET).pdb save_items_header.o

.PHONY: all check clean
