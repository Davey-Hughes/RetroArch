# The raster poll interface: RetroArch's contract checker
# (gfx/video_raster.c) against synthetic call sequences.

CORE_DIR          := ../../..
LIBRETRO_COMM_DIR := $(CORE_DIR)/libretro-common

TARGET := raster_poll_test

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

OBJS := raster_poll_test.o video_raster.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

raster_poll_test.o: raster_poll_test.c
	$(CC) $(CFLAGS) -std=gnu99 -c -o $@ $<

# Built here, not next to the source, so no other build shares it
video_raster.o: $(CORE_DIR)/gfx/video_raster.c
	$(CC) $(CFLAGS) $(C89) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean
