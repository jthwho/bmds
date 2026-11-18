#!/bin/sh
# make-romdata-seeds.sh BUILD DIR: romdata_fuzz's seeds (devplan/rom.md §12f) from a firmware build
# (its build/rom: the icon pack, the zone bundle) and the repository (the gamepad database), each
# with the byte that picks its reader in front: 0 the icon pack, 1 the zone bundle, 2 the gamepads
# (then a vendor and product, little-endian: 0x0079, 0x0011, a line the database has).
set -eu
build=${1:?usage: make-romdata-seeds.sh BUILD DIR}
out=${2:?usage: make-romdata-seeds.sh BUILD DIR}
root=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$out"
{ printf '\000'; cat "$build/rom/bmds.icons"; } > "$out/icons"
{ printf '\001'; cat "$build/rom/zoneinfo.bin"; } > "$out/zones"
{ printf '\002\171\000\021\000'; cat "$root/rom/gamepads.txt"; } > "$out/gamepads"
