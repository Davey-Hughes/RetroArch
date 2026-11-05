CC ?= cc
ROOT := ../../..
TARGET := save_items_test
IO_TARGET := save_items_io_test
IO_NT_TARGET := save_items_io_test_nothreads
BENCH_TARGET := save_items_fill_bench
EXE ?=
COMMON := $(ROOT)/save_items.c \
          $(ROOT)/libretro-common/file/file_path.c \
          $(ROOT)/libretro-common/lists/string_list.c \
          $(ROOT)/libretro-common/compat/compat_strl.c \
          $(ROOT)/libretro-common/compat/compat_posix_string.c \
          $(ROOT)/libretro-common/encodings/encoding_utf.c \
          $(ROOT)/libretro-common/time/rtime.c
SOURCES := save_items_test.c $(COMMON)
# save.c and save_items.c are included by the test, which stubs
# save.c's streams.
IO_SOURCES := save_items_io_test.c $(filter-out $(ROOT)/save_items.c,$(COMMON)) \
              $(ROOT)/libretro-common/rthreads/rthreads.c \
              $(ROOT)/libretro-common/rthreads/retro_eventcount.c \
              $(ROOT)/libretro-common/encodings/encoding_crc32.c \
              $(ROOT)/libretro-common/features/features_cpu.c
# Real file reads: a measurement, built by hand (make bench), never by
# check.
BENCH_SOURCES := save_items_fill_bench.c $(COMMON) \
                 $(ROOT)/libretro-common/file/file_path_io.c \
                 $(ROOT)/libretro-common/streams/file_stream.c \
                 $(ROOT)/libretro-common/string/rstrtod.c \
                 $(ROOT)/libretro-common/compat/fopen_utf8.c \
                 $(ROOT)/libretro-common/vfs/vfs_implementation.c \
                 $(ROOT)/libretro-common/encodings/encoding_crc32.c \
                 $(ROOT)/libretro-common/features/features_cpu.c
CFLAGS += -std=gnu89 -Wall -g -O1 \
          -I$(ROOT) -I$(ROOT)/libretro-common/include
IO_CFLAGS := -DHAVE_THREADS -DHAVE_COMPRESSION
# The same without threads: only its own lane.
IO_NT_CFLAGS := -DHAVE_COMPRESSION
# The header alone, held to the C89 the C89_BUILD lane uses.
C89_CFLAGS := -std=c89 -pedantic -Werror=pedantic -Wall -Wextra -Werror \
              -Wno-long-long -I$(ROOT)/libretro-common/include
# clang -std=c89 -pedantic flags the _Bool that stdbool.h gives libretro.h.
ifneq ($(findstring clang,$(shell $(CC) --version 2>/dev/null)),)
C89_CFLAGS += -Wno-c99-extensions
# Counters the shared stubs set and only threaded lanes read.
IO_NT_CFLAGS += -Wno-unknown-warning-option -Wno-unused-but-set-global
endif
ifeq ($(EXE),)
IO_LDLIBS := -lpthread
endif

ifneq ($(SANITIZER),)
CFLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
LDFLAGS += -fsanitize=$(SANITIZER)
endif

all: $(TARGET)$(EXE) $(IO_TARGET)$(EXE) $(IO_NT_TARGET)$(EXE)

save_items_header.o: save_items_header.c $(ROOT)/libretro-common/include/libretro.h
	$(CC) $(C89_CFLAGS) -c save_items_header.c -o $@

$(TARGET)$(EXE): $(SOURCES) save_items_header.o $(ROOT)/save_items.h \
                 $(ROOT)/libretro-common/include/libretro.h
	$(CC) $(CFLAGS) $(SOURCES) save_items_header.o $(LDFLAGS) -o $@

$(IO_TARGET)$(EXE): $(IO_SOURCES) $(ROOT)/save.c $(ROOT)/save_items.c \
                    $(ROOT)/save_items.h \
                    $(ROOT)/autosave.h
	$(CC) $(CFLAGS) $(IO_CFLAGS) $(IO_SOURCES) $(LDFLAGS) $(IO_LDLIBS) -o $@

$(IO_NT_TARGET)$(EXE): $(IO_SOURCES) $(ROOT)/save.c $(ROOT)/save_items.c \
                       $(ROOT)/save_items.h \
                       $(ROOT)/autosave.h
	$(CC) $(CFLAGS) $(IO_NT_CFLAGS) $(IO_SOURCES) $(LDFLAGS) $(IO_LDLIBS) -o $@

bench: $(BENCH_TARGET)$(EXE)

$(BENCH_TARGET)$(EXE): $(BENCH_SOURCES) $(ROOT)/save.c $(ROOT)/save_items.h
	$(CC) $(CFLAGS) -O2 $(BENCH_SOURCES) $(LDFLAGS) -o $@

check: $(TARGET)$(EXE) $(IO_TARGET)$(EXE) $(IO_NT_TARGET)$(EXE)
	./$(TARGET)$(EXE)
	./$(IO_TARGET)$(EXE)
	./$(IO_NT_TARGET)$(EXE)

clean:
	rm -f $(TARGET) $(TARGET).exe $(TARGET).pdb save_items_header.o \
	      $(IO_TARGET) $(IO_TARGET).exe $(IO_TARGET).pdb \
	      $(IO_NT_TARGET) $(IO_NT_TARGET).exe $(IO_NT_TARGET).pdb \
	      $(BENCH_TARGET) $(BENCH_TARGET).exe $(BENCH_TARGET).pdb

.PHONY: all bench check clean
