TARGET := libxr_test_layer.so

CFLAGS  += -O2 -g -fPIC -Wall -std=gnu11 -fvisibility=hidden
LDFLAGS += -shared -Wl,--no-undefined -lvulkan -lz -lpthread -lm

all: $(TARGET)

$(TARGET): xr_test_layer.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
