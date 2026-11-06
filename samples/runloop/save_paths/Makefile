TARGET := save_paths_test
ROOT := ../../..
LIBRETRO_COMM_DIR := $(ROOT)/libretro-common
SOURCES := save_paths_test.c \
           $(LIBRETRO_COMM_DIR)/compat/compat_strl.c \
           $(LIBRETRO_COMM_DIR)/compat/compat_posix_string.c \
           $(LIBRETRO_COMM_DIR)/compat/fopen_utf8.c \
           $(LIBRETRO_COMM_DIR)/encodings/encoding_utf.c \
           $(LIBRETRO_COMM_DIR)/file/file_path.c \
           $(LIBRETRO_COMM_DIR)/file/file_path_io.c \
           $(LIBRETRO_COMM_DIR)/file/retro_dirent.c \
           $(LIBRETRO_COMM_DIR)/lists/dir_list.c \
           $(LIBRETRO_COMM_DIR)/lists/string_list.c \
           $(LIBRETRO_COMM_DIR)/queues/message_queue.c \
           $(LIBRETRO_COMM_DIR)/rthreads/rthreads.c \
           $(LIBRETRO_COMM_DIR)/streams/file_stream.c \
           $(LIBRETRO_COMM_DIR)/string/stdstring.c \
           $(LIBRETRO_COMM_DIR)/vfs/vfs_implementation.c
CFLAGS += -O2 -g1 -std=gnu89 -Wall -flto \
          -ffunction-sections -fdata-sections \
          -DHAVE_NETWORKING \
          -I$(ROOT) -I$(ROOT)/menu -I$(LIBRETRO_COMM_DIR)/include $(EXTRA_CFLAGS)
LDFLAGS += -flto -Wl,--gc-sections -lpthread
ifneq ($(SANITIZER),)
CFLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
LDFLAGS += -fsanitize=$(SANITIZER)
endif

all: $(TARGET)

$(TARGET): $(SOURCES) $(ROOT)/runloop.c $(ROOT)/runloop.h
	$(CC) $(CFLAGS) $(SOURCES) -o $@ $(LDFLAGS)

check: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) $(TARGET).exe
	rm -rf save_paths_test_files

.PHONY: all check clean
