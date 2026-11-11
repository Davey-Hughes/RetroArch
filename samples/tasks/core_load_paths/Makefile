# See build.sh: this harness links the shipping RetroArch objects of a
# build with the load check, main() replaced. One mode a run, since a
# startup load happens once a process.
TARGET := core_load_paths_test
MODES  := startup startup-always startup-outside startup-content \
          startup-needs-content \
          null-seed null-refused staged staged-threaded playlist subsystem \
          playlist-sub netplay-sub probe seed runahead load-once park-ends

all:
	@MAKE_ARGS="$(MAKE_ARGS)" ./build.sh

check: all
	@for m in $(MODES); do \
	   ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
	   UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
	   ./$(TARGET) $$m > check-$$m.log 2>&1 || { grep -vE '^\[(INFO|DEBUG|WARN|ERROR)\]' check-$$m.log; rm -f check-$$m.log; exit 1; }; \
	   grep -vE '^\[(INFO|DEBUG|WARN|ERROR)\]' check-$$m.log; rm -f check-$$m.log; \
	done

clean:
	rm -f $(TARGET) $(TARGET).exe harness_core.so harness_core_b.so \
	      needs_content_core.so retroarch_nomain.o harness_main.o \
	      core_signature_testkey.o ui_cocoa_nomain.o \
	      retroarch_nomain.d harness_main.d core_signature_testkey.d \
	      ui_cocoa_nomain.d \
	      check-*.log
	rm -rf harness_core.so.dSYM harness_core_b.so.dSYM \
	       needs_content_core.so.dSYM

.PHONY: all check clean
