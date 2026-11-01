CC ?= cc
ROOT := ../../..
TARGET := autosave_snapshot_test
SOURCES := autosave_snapshot_test.c $(ROOT)/save_items.c \
           $(ROOT)/libretro-common/file/file_path.c \
           $(ROOT)/libretro-common/file/file_path_io.c \
           $(ROOT)/libretro-common/streams/file_stream.c \
           $(ROOT)/libretro-common/vfs/vfs_implementation.c \
           $(ROOT)/libretro-common/compat/compat_strl.c \
           $(ROOT)/libretro-common/compat/fopen_utf8.c \
           $(ROOT)/libretro-common/encodings/encoding_utf.c \
           $(ROOT)/libretro-common/encodings/encoding_crc32.c \
           $(ROOT)/libretro-common/lists/string_list.c \
           $(ROOT)/libretro-common/string/rstrtod.c \
           $(ROOT)/libretro-common/rthreads/rthreads.c \
           $(ROOT)/libretro-common/rthreads/retro_eventcount.c \
           $(ROOT)/libretro-common/features/features_cpu.c \
           $(ROOT)/libretro-common/time/rtime.c
CFLAGS += -std=gnu89 -Wall -Wextra -Wno-unused-parameter -g -O1 -DHAVE_THREADS \
          -ffunction-sections -fdata-sections \
          -I$(ROOT) -I$(ROOT)/libretro-common/include
LDFLAGS += -Wl,--gc-sections
LDLIBS += -lpthread

# PE linkers retain unrelated external save.c functions without LTO.
ifneq ($(filter MSYS% MINGW% CYGWIN%,$(shell uname -s)),)
CFLAGS += -flto
endif

ifneq ($(SANITIZER),)
CFLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
LDFLAGS += -fsanitize=$(SANITIZER)
endif

all: $(TARGET)

$(TARGET): $(SOURCES) $(ROOT)/save.c $(ROOT)/autosave.h $(ROOT)/save_items.h
	$(CC) $(CFLAGS) $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

check: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) $(TARGET).exe

.PHONY: all check clean
