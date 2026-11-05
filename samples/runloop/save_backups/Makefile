# save_backups.c, the safety-copy helper (#included by the test, which
# sets its clock), over the real file stream and VFS.
#
#   make check
#   make check SANITIZER=address,undefined
#   make CC=x86_64-w64-mingw32-gcc EXE=.exe LDFLAGS=-static

ROOT              := ../../..
LIBRETRO_COMM_DIR := $(ROOT)/libretro-common
TARGET            := save_backups_test
EXE               :=

SOURCES := save_backups_test.c \
	$(LIBRETRO_COMM_DIR)/lists/string_list.c \
	$(LIBRETRO_COMM_DIR)/lists/dir_list.c \
	$(LIBRETRO_COMM_DIR)/file/retro_dirent.c \
	$(LIBRETRO_COMM_DIR)/file/file_path.c \
	$(LIBRETRO_COMM_DIR)/file/file_path_io.c \
	$(LIBRETRO_COMM_DIR)/streams/file_stream.c \
	$(LIBRETRO_COMM_DIR)/vfs/vfs_implementation.c \
	$(LIBRETRO_COMM_DIR)/compat/fopen_utf8.c \
	$(LIBRETRO_COMM_DIR)/compat/compat_posix_string.c \
	$(LIBRETRO_COMM_DIR)/compat/compat_strl.c \
	$(LIBRETRO_COMM_DIR)/compat/compat_strcasestr.c \
	$(LIBRETRO_COMM_DIR)/string/stdstring.c \
	$(LIBRETRO_COMM_DIR)/string/rstrtod.c \
	$(LIBRETRO_COMM_DIR)/encodings/encoding_utf.c \
	$(LIBRETRO_COMM_DIR)/time/rtime.c

# What the Win32 VFS reaches with threads.
THREAD_SOURCES := $(LIBRETRO_COMM_DIR)/rthreads/rthreads.c \
	$(LIBRETRO_COMM_DIR)/rthreads/retro_eventcount.c \
	$(LIBRETRO_COMM_DIR)/features/features_cpu.c

CFLAGS += -std=gnu89 -Wall -g -O1 \
          -I$(ROOT) -I$(LIBRETRO_COMM_DIR)/include

ifneq ($(SANITIZER),)
CFLAGS  += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
LDFLAGS += -fsanitize=$(SANITIZER)
endif

all: $(TARGET)$(EXE) $(TARGET)_nothreads$(EXE)

$(TARGET)$(EXE): $(SOURCES) $(ROOT)/save_backups.c $(ROOT)/save_backups.h
	$(CC) $(CFLAGS) -DHAVE_THREADS $(SOURCES) $(THREAD_SOURCES) $(LDFLAGS) -lpthread -o $@

$(TARGET)_nothreads$(EXE): $(SOURCES) $(ROOT)/save_backups.c $(ROOT)/save_backups.h
	$(CC) $(CFLAGS) $(SOURCES) $(LDFLAGS) -o $@

check: all
	./$(TARGET)$(EXE)
	./$(TARGET)_nothreads$(EXE)

clean:
	rm -f $(TARGET) $(TARGET).exe $(TARGET)_nothreads $(TARGET)_nothreads.exe
	rm -rf save_backups_test_files

.PHONY: all check clean
