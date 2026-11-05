#!/bin/sh
# Builds the save item ordering harness.
#
# Like cloudsync_unload, this links the SHIPPING objects of a built
# RetroArch with only main() replaced. The core it loads (save_core.so)
# is built here too.
#
# Requires a completed NON-Qt build, from the repo root:
#
#   ./configure --disable-qt && make
#   samples/runloop/save_ordering/build.sh
#
# The compile and link lines are the project's own, taken from make -n.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
out=samples/runloop/save_ordering

cd "$root"

if [ ! -f obj-unix/release/retroarch.o ]; then
   echo "build RetroArch first: ./configure --disable-qt && make" >&2
   exit 1
fi

cc_line=$(mktemp)
ld_line=$(mktemp)
dry=$(mktemp)
trap 'rm -f "$cc_line" "$ld_line" "$dry"' EXIT

# As if retroarch.c had changed (-W): make prints the compile and link
# lines without touching or removing a file, so the built retroarch
# stays. Only this directory's own outputs are written.
make -n -W retroarch.c 2>/dev/null > "$dry" \
   || { echo "make -n failed (reconfigure?)" >&2; exit 1; }
grep -E '\-o obj-unix/release/retroarch\.o' "$dry" | head -1 > "$cc_line"
if [ ! -s "$cc_line" ]; then
   echo "could not determine the compile command for retroarch.o" >&2
   exit 1
fi

sed "s#-o obj-unix/release/retroarch\.o#-Dmain=rarch_harness_unused_main -o $out/retroarch_nomain.o#" \
   "$cc_line" | sh

sed -e "s#-o obj-unix/release/retroarch\.o#-o $out/harness_main.o#" \
    -e "s# retroarch\.c# $out/save_ordering_test.c#" \
   "$cc_line" | sh

grep -E ' -o retroarch ' "$dry" | tail -1 > "$ld_line"
if [ ! -s "$ld_line" ]; then
   echo "could not determine the link command" >&2
   exit 1
fi

sed -e "s#obj-unix/release/retroarch\.o#$out/retroarch_nomain.o $out/harness_main.o#" \
    -e "s#-o retroarch #-o $out/save_ordering_test #" \
   "$ld_line" | sh

core_cc=$(sed 's/ .*//' "$cc_line")
$core_cc -O1 -g -shared -fPIC -Ilibretro-common/include \
   -o $out/save_core.so $out/save_core.c

echo "built $out/save_ordering_test and $out/save_core.so"
