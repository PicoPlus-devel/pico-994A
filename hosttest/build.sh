#!/usr/bin/env bash
# Build the host harness: the TI-99/4A core from ti99/ compiled for Linux, plus the
# harness in this directory. See hosttest/README.md.
#
# ti99_fileio.c and cassette.c are left out because they need FatFs; fileio_host.c and
# stubs in ti99_host.c stand in for them. SANITIZE=1 adds AddressSanitizer. Warnings are
# off because the upstream core is not warning-clean.
set -euo pipefail
cd "$(dirname "$0")/.."   # repo root

T=ti99
CFLAGS=(
  -O2 -g -w
  -DBIOS_DIR="\"$PWD/bios\""
  -I "$T" -I "$T/cpu/tms9900" -I "$T/cpu/tms9918a" -I "$T/cpu/sn76496" -I "$T/rpk"
)
if [ -n "${SANITIZE:-}" ]; then CFLAGS+=(-O1 -fsanitize=address); fi

HOST_SRC=(
  hosttest/ti99_host.c
  hosttest/fileio_host.c
)
CORE_SRC=(
  "$T/cpu/tms9900/tms9900.c" "$T/cpu/tms9900/tms9901.c" "$T/cpu/tms9918a/tms9918a.c"
  "$T/cpu/sn76496/sn76496.c" "$T/cpu/sn76496/sn76496_shim.c"
  "$T/SAMS.c" "$T/speech.c" "$T/disk.c" "$T/pcode.c"
  "$T/rpk/rpk.c" "$T/rpk/lowzip.c" "$T/rpk/yxml.c" "$T/CRC32.c"
  "$T/ti99_pico.c"
)

gcc "${CFLAGS[@]}" -o hosttest/ti99_host "${HOST_SRC[@]}" "${CORE_SRC[@]}" -lm
echo "built hosttest/ti99_host"
