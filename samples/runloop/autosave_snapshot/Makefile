CC ?= cc
ROOT := ../../..
LRC := $(ROOT)/libretro-common
TARGET := autosave_snapshot_test
SOURCES := autosave_snapshot_test.c $(ROOT)/libretro-common/rthreads/rthreads.c \
           $(ROOT)/libretro-common/rthreads/retro_eventcount.c \
           $(ROOT)/libretro-common/features/features_cpu.c \
           $(ROOT)/libretro-common/time/rtime.c

# autosave_io_test: real files through file_stream, rzip and a test VFS.
IO_TARGET := autosave_io_test
IO_NOTHREADS_TARGET := autosave_io_test_nothreads
IO_SOURCES := autosave_io_test.c $(ROOT)/save_backups.c \
              $(LRC)/compat/compat_strl.c \
              $(LRC)/compat/fopen_utf8.c \
              $(LRC)/encodings/encoding_crc32.c \
              $(LRC)/encodings/encoding_deflate.c \
              $(LRC)/encodings/encoding_utf.c \
              $(LRC)/features/features_cpu.c \
              $(LRC)/file/file_path.c \
              $(LRC)/file/file_path_io.c \
              $(LRC)/file/retro_dirent.c \
              $(LRC)/lists/dir_list.c \
              $(LRC)/lists/string_list.c \
              $(LRC)/streams/file_stream.c \
              $(LRC)/streams/interface_stream.c \
              $(LRC)/streams/memory_stream.c \
              $(LRC)/streams/rzip_stream.c \
              $(LRC)/streams/trans_stream.c \
              $(LRC)/streams/trans_stream_deflate.c \
              $(LRC)/streams/trans_stream_pipe.c \
              $(LRC)/string/stdstring.c \
              $(LRC)/time/rtime.c \
              $(LRC)/vfs/vfs_implementation.c
IO_THREAD_SOURCES := $(LRC)/rthreads/rthreads.c \
                     $(LRC)/rthreads/retro_eventcount.c

BASE_CFLAGS := $(CFLAGS) -std=gnu89 -Wall -g -O1 \
          -ffunction-sections -fdata-sections \
          -I$(ROOT) -I$(ROOT)/libretro-common/include
CFLAGS := $(BASE_CFLAGS) -Wextra -DHAVE_THREADS
IO_CFLAGS := $(BASE_CFLAGS) -Wextra -Wno-unused-parameter -DHAVE_COMPRESSION
LDFLAGS += -Wl,--gc-sections
LDLIBS += -lpthread

# PE linkers retain unrelated external save.c functions without LTO.
ifneq ($(filter MSYS% MINGW% CYGWIN%,$(shell uname -s)),)
CFLAGS += -flto
IO_CFLAGS += -flto
endif
ifneq ($(findstring mingw,$(CC)),)
CFLAGS += -flto
IO_CFLAGS += -flto
endif

ifneq ($(SANITIZER),)
CFLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
IO_CFLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
LDFLAGS += -fsanitize=$(SANITIZER)
endif

all: $(TARGET) $(IO_TARGET) $(IO_NOTHREADS_TARGET)

$(TARGET): $(SOURCES) $(ROOT)/save.c $(ROOT)/autosave.h
	$(CC) $(CFLAGS) $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

$(IO_TARGET): $(IO_SOURCES) $(IO_THREAD_SOURCES) $(ROOT)/save.c $(ROOT)/autosave.h
	$(CC) $(IO_CFLAGS) -DHAVE_THREADS $(IO_SOURCES) $(IO_THREAD_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

$(IO_NOTHREADS_TARGET): $(IO_SOURCES) $(ROOT)/save.c $(ROOT)/autosave.h
	$(CC) $(IO_CFLAGS) $(IO_SOURCES) $(LDFLAGS) -o $@

check: all
	./$(TARGET)
	./$(IO_TARGET)
	./$(IO_NOTHREADS_TARGET)

clean:
	rm -f $(TARGET) $(TARGET).exe $(IO_TARGET) $(IO_TARGET).exe \
	      $(IO_NOTHREADS_TARGET) $(IO_NOTHREADS_TARGET).exe
	rm -f game.srm game.srm.tmp game.srm.auto.tmp
	rm -rf backups blk cfg

.PHONY: all check clean
