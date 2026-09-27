# gfx/common/drm_scanout.c: a connector's scanout timing and VRR state
# by name, against a fake sysfs tree and a stubbed libdrm. Needs the
# libdrm headers only; Linux-samples-gfx.yml runs it.
TARGET    := drm_scanout_test
REPO_ROOT := ../../..
SOURCES   := drm_scanout_test.c $(REPO_ROOT)/gfx/common/drm_scanout.c
CFLAGS    += -Wall -std=gnu99 -g -O0 -I$(REPO_ROOT) \
             -I$(REPO_ROOT)/libretro-common/include \
             $(shell pkg-config --cflags libdrm 2>/dev/null || echo -I/usr/include/libdrm)

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
