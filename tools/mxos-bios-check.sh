#!/bin/sh
# mxos-bios-check.sh <llvm bin dir> <file>...
#
# mx OS plan steps 2h-3 and 2h-3c: an MXOS program, and libmxk, call the
# kernel only, never the BIOS. Fails (exit 1) when a file (an ELF, an object or
# an archive) has a syscall instruction anywhere but in mx__no_kernel_exit
# (libmxk's stop when no kernel started the program, ee/src/base/start.c).
# maxsdk's Makefile runs it on libmxk, mk/maxsdk.mk on every MXOS program it
# links.
set -e
llvm=$1
shift
status=0
dis=$(mktemp)
trap 'rm -f "$dis"' EXIT
for f in "$@"; do
  "$llvm/llvm-objdump" -d --mcpu=r5900 --no-show-raw-insn "$f" > "$dis"
  bad=$(awk '
    /file format/ { obj = $1 }
    /^[0-9a-f]+ <.*>:$/ { fn = $2; gsub(/[<>:]/, "", fn) }
    $2 == "syscall" && fn != "mx__no_kernel_exit" { print obj " " fn }
  ' "$dis" | sort -u)
  if [ -n "$bad" ]; then
    echo "$f: a BIOS call in an MXOS program (mx OS plan 2h-3c: MX_TARGET=standalone for one that makes them):" >&2
    echo "$bad" | sed 's/^/  syscall in /' >&2
    status=1
  fi
done
exit $status
