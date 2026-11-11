#!/bin/sh
# libk-bios-check.sh <llvm bin dir> <libmxk.a> [allowed stub...]
#
# mx OS plan step 2h-3: libmxk calls the kernel only. Fails (exit 1) when an
# object of the library other than syscalls.S calls one of syscalls.S's BIOS
# stubs that is not named as allowed, or has a syscall instruction of its own.
set -e
llvm=$(cd "$1" && pwd)
lib=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
shift 2
dir=$(mktemp -d)
trap 'rm -r "$dir"' EXIT
(cd "$dir" && "$llvm/llvm-ar" x "$lib")
stubs=$("$llvm/llvm-nm" --defined-only "$dir/syscalls.S.o" | awk '{print $3}')
status=0
for o in "$dir"/*.o; do
  [ "$(basename "$o")" = syscalls.S.o ] && continue
  for s in $("$llvm/llvm-nm" -u "$o" | awk '{print $2}'); do
    echo "$stubs" | grep -qx "$s" || continue
    ok=0
    for a in "$@"; do [ "$a" = "$s" ] && ok=1; done
    if [ $ok = 0 ]; then
      echo "libmxk calls the BIOS: $(basename "$o" .o) calls $s (mx OS plan 2h-3)" >&2
      status=1
    fi
  done
  if "$llvm/llvm-objdump" -d --mcpu=r5900 "$o" | grep -qw syscall; then
    echo "libmxk: $(basename "$o" .o) has a syscall of its own (mx OS plan 2h-3)" >&2
    status=1
  fi
done
exit $status
