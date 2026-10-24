# Steam's controller lists, read as SDL reads them, through the joypad
# drivers' own steam_lists.h (no libudev or evdev types). Plain C and
# libretro-common only, so it builds and runs with mingw too.
#
#   make check
#   make check SANITIZER=address,undefined
#   make check CC=x86_64-w64-mingw32-gcc EXE=.exe RUNNER=wine

CC     ?= cc
CFLAGS ?= -O1 -g1
LC         := ../../../libretro-common
COMMON_DIR := ../../../input/common
CFLAGS += -Wall -Wextra -std=gnu89 -I$(LC)/include -I$(COMMON_DIR)
ifneq ($(SANITIZER),)
CFLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
TARGET = steam_lists_test$(EXE)

SOURCES := steam_lists_test.c \
   $(LC)/streams/file_stream.c $(LC)/vfs/vfs_implementation.c \
   $(LC)/file/file_path.c $(LC)/file/file_path_io.c \
   $(LC)/string/stdstring.c $(LC)/string/rstrtod.c \
   $(LC)/encodings/encoding_utf.c $(LC)/compat/fopen_utf8.c \
   $(LC)/compat/compat_strl.c $(LC)/time/rtime.c

all: $(TARGET)

$(TARGET): $(SOURCES) $(COMMON_DIR)/steam_lists.h
	$(CC) $(CFLAGS) $(SOURCES) -o $@ $(LDFLAGS)

check: $(TARGET)
	$(RUNNER) ./$(TARGET)

clean:
	rm -f steam_lists_test steam_lists_test.exe
	rm -rf steam_lists_test_files

.PHONY: all check clean
