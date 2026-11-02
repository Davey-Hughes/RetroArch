# See build.sh: this harness links the shipping RetroArch objects with
# only main() replaced, so it needs the project's own build rather than
# a source list of its own.
TARGET := save_ordering_test

all:
	@./build.sh

clean:
	rm -f $(TARGET) retroarch_nomain.o retroarch_nomain.d \
	      harness_main.o harness_main.d save_core.so

.PHONY: all clean
