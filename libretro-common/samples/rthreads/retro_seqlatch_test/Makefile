# rthreads/retro_seqlatch.h: a record one thread publishes while others
# read it, neither ever waiting on the other.

TARGET    := retro_seqlatch_test
CORE_DIR  := ../../..

SOURCES := retro_seqlatch_test.c
CFLAGS  += -Wall -std=gnu99 -g1 -O1 -I$(CORE_DIR)/include
LDFLAGS += -lpthread
OBJS    := $(SOURCES:.c=.o)
ifneq ($(SANITIZER),)
   CFLAGS  := -fsanitize=$(SANITIZER) -fno-omit-frame-pointer $(CFLAGS)
   LDFLAGS := -fsanitize=$(SANITIZER) $(LDFLAGS)
endif

all: $(TARGET)
%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS)
$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)
check: $(TARGET)
	./$(TARGET)
clean:
	rm -f $(TARGET) $(OBJS)
.PHONY: all check clean
