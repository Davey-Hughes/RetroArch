# gfx/common/wayland_beam.c: the presentation timing the Wayland
# context publishes and the display server reads, across threads.
TARGET    := wayland_beam_test
REPO_ROOT := ../../..
LRC       := $(REPO_ROOT)/libretro-common
SOURCES   := wayland_beam_test.c \
             $(REPO_ROOT)/gfx/common/wayland_beam.c \
             $(LRC)/rthreads/rthreads.c
CFLAGS    += -Wall -std=gnu99 -g -O1 -DHAVE_THREADS \
             -I$(REPO_ROOT) -I$(LRC)/include
LDFLAGS   += -lpthread -lrt

ifneq ($(SANITIZER),)
   CFLAGS  := -fsanitize=$(SANITIZER) -fno-omit-frame-pointer $(CFLAGS)
   LDFLAGS := -fsanitize=$(SANITIZER) $(LDFLAGS)
endif

all: $(TARGET)
$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
check: $(TARGET)
	./$(TARGET)
clean:
	rm -f $(TARGET)
.PHONY: all check clean
