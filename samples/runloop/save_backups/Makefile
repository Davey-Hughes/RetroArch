# save_backups.c, the safety-copy helper (#included by the test, which
# sets its clock), over the real file stream and VFS.
#
#   make check
#   make check SANITIZER=address,undefined
#   make CC=x86_64-w64-mingw32-gcc EXE=.exe LDFLAGS=-static
#
# _pathmax has the Windows path limit a copy's name fits to.

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
	$(LIBRETRO_COMM_DIR)/encodings/encoding_crc32.c \
	$(LIBRETRO_COMM_DIR)/encodings/encoding_utf.c \
	$(LIBRETRO_COMM_DIR)/features/features_cpu.c \
	$(LIBRETRO_COMM_DIR)/formats/json/rjson.c \
	$(LIBRETRO_COMM_DIR)/time/rtime.c

# What the Win32 VFS reaches with threads.
THREAD_SOURCES := $(LIBRETRO_COMM_DIR)/rthreads/rthreads.c \
	$(LIBRETRO_COMM_DIR)/rthreads/retro_eventcount.c

# RARCH_INTERNAL, as the frontend builds it, for path_resolve_realpath()
CFLAGS += -std=gnu89 -Wall -g -O1 -DRARCH_INTERNAL \
          -I$(ROOT) -I$(LIBRETRO_COMM_DIR)/include

ifneq ($(SANITIZER),)
CFLAGS  += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
LDFLAGS += -fsanitize=$(SANITIZER)
endif

all: $(TARGET)$(EXE) $(TARGET)_nothreads$(EXE) $(TARGET)_pathmax$(EXE)

$(TARGET)$(EXE): $(SOURCES) $(ROOT)/save_backups.c $(ROOT)/save_backups.h
	$(CC) $(CFLAGS) -DHAVE_THREADS $(SOURCES) $(THREAD_SOURCES) $(LDFLAGS) -lpthread -o $@

$(TARGET)_nothreads$(EXE): $(SOURCES) $(ROOT)/save_backups.c $(ROOT)/save_backups.h
	$(CC) $(CFLAGS) $(SOURCES) $(LDFLAGS) -o $@

$(TARGET)_pathmax$(EXE): $(SOURCES) $(ROOT)/save_backups.c $(ROOT)/save_backups.h
	$(CC) $(CFLAGS) -DSAVE_BACKUPS_PATH_MAX=259 $(SOURCES) $(LDFLAGS) -o $@

check: all
	./$(TARGET)$(EXE)
	./$(TARGET)_nothreads$(EXE)
	./$(TARGET)_pathmax$(EXE)

clean:
	rm -f $(TARGET) $(TARGET).exe $(TARGET)_nothreads $(TARGET)_nothreads.exe
	rm -f $(TARGET)_pathmax $(TARGET)_pathmax.exe
	rm -rf save_backups_test_files

.PHONY: all check clean
