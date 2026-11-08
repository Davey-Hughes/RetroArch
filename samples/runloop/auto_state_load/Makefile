TARGET := auto_state_load_test
ROOT := ../../..
SOURCES := auto_state_load_test.c $(ROOT)/command.c \
           $(ROOT)/libretro-common/file/file_path.c \
           $(ROOT)/libretro-common/file/retro_dirent.c \
           $(ROOT)/libretro-common/lists/dir_list.c \
           $(ROOT)/libretro-common/lists/string_list.c \
           $(ROOT)/libretro-common/streams/file_stream.c \
           $(ROOT)/libretro-common/string/stdstring.c \
           $(ROOT)/libretro-common/vfs/vfs_implementation.c \
           $(ROOT)/libretro-common/compat/fopen_utf8.c \
           $(ROOT)/libretro-common/encodings/encoding_utf.c \
           $(ROOT)/libretro-common/compat/compat_strl.c
CFLAGS += -O2 -g1 -std=gnu89 -Wall -flto \
          -ffunction-sections -fdata-sections \
          -I$(ROOT) -I$(ROOT)/menu -I$(ROOT)/libretro-common/include $(EXTRA_CFLAGS)
LDFLAGS += -flto -Wl,--gc-sections
ifneq ($(SANITIZER),)
CFLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
LDFLAGS += -fsanitize=$(SANITIZER)
endif

all: $(TARGET)

$(TARGET): $(SOURCES) $(ROOT)/runloop.c $(ROOT)/runloop.h
	$(CC) $(CFLAGS) $(SOURCES) -o $@ $(LDFLAGS)

# The states a lane leaves when it fails go too
check: $(TARGET)
	./$(TARGET); s=$$?; rm -rf asl_states; exit $$s

clean:
	rm -f $(TARGET) $(TARGET).exe
	rm -rf asl_states

.PHONY: all check clean
