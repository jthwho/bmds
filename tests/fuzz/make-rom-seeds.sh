#!/bin/sh
# make-rom-seeds.sh DIR: the ROM fuzzer's seeds (devplan/rom.md §12c): small partitions made by
# tools/mkrom.py from tests/host/rom, with a one-sector log so they stay under 128 KB.
set -eu
out=${1:?usage: make-rom-seeds.sh DIR}
root=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$out"
mkrom() { python3 "$root/tools/mkrom.py" image "$@"; }
mkrom "$out/tree.rom" --log-bytes 4k --dir "=$root/tests/host/rom" --link docs/hello-link=/rom/hello.txt \
        --align 4k=docs/*
mkrom "$out/one.rom" --log-bytes 4k --file hello.txt="$root/tests/host/rom/hello.txt"
mkrom "$out/empty.rom" --log-bytes 4k
