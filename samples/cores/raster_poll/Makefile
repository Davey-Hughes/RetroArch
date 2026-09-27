# The raster poll interface: RetroArch's contract checker
# (gfx/video_raster.c) against synthetic call sequences and against a
# software core that polls the raster, that core as a libretro core
# RetroArch can load (raster_poll_libretro.so), and a libretro frontend
# that checks any core's raster polls (raster_poll_conform).

CORE_DIR          := ../../..
LIBRETRO_COMM_DIR := $(CORE_DIR)/libretro-common

TARGET := raster_poll_test
CORE   := raster_poll_libretro.so
CONFORM := raster_poll_conform

INCDIRS := -I$(LIBRETRO_COMM_DIR)/include
# RetroArch's linux-c89 lane
C89     := -std=c89 -pedantic -Werror=pedantic -Wno-long-long \
           -Werror=declaration-after-statement -Wno-variadic-macros

CFLAGS  += -Wall $(INCDIRS)

ifneq ($(SANITIZER),)
CFLAGS  += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer -g
LDFLAGS += -fsanitize=$(SANITIZER)
else
CFLAGS  += -g -O1
endif

OBJS := raster_poll_test.o video_raster.o raster_poll_core.o

all: $(TARGET) $(CORE) $(CONFORM)

$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

raster_poll_test.o: raster_poll_test.c
	$(CC) $(CFLAGS) -std=gnu99 -c -o $@ $<

# Built here, not next to the source, so no other build shares it
video_raster.o: $(CORE_DIR)/gfx/video_raster.c
	$(CC) $(CFLAGS) $(C89) -c -o $@ $<

raster_poll_core.o: raster_poll_core.c
	$(CC) $(CFLAGS) $(C89) -c -o $@ $<

# Loaded by RetroArch, so never sanitized
$(CORE): raster_poll_core.c
	$(CC) -Wall $(INCDIRS) $(C89) -O2 -fPIC -shared -o $@ $<

$(CONFORM): raster_poll_conform.o video_raster.o
	$(CC) -o $@ raster_poll_conform.o video_raster.o $(LDFLAGS) -ldl -pthread

raster_poll_conform.o: raster_poll_conform.c
	$(CC) $(CFLAGS) $(C89) -c -o $@ $<

clean:
	rm -f $(TARGET) $(CORE) $(CONFORM) $(OBJS) raster_poll_conform.o

.PHONY: all clean
