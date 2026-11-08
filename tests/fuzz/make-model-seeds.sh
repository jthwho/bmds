#!/bin/sh
# make-model-seeds.sh DIR [MESH3D]: model3d_fuzz's seeds (devplan/3d.md §12d), small .bm3d files
# made by bmds-mesh3d (default build-host/bmds-mesh3d): the teapot coarse, with and without its
# normals, uvs and edges; a cube of named groups with uvs; a faceted one; a lone triangle.
set -eu
out=${1:?usage: make-model-seeds.sh DIR [MESH3D]}
root=$(cd "$(dirname "$0")/../.." && pwd)
mesh3d=${2:-$root/build-host/bmds-mesh3d}
teapot=$root/thirdparty/teapot/teapot.bpt
mkdir -p "$out"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
"$mesh3d" --detail 1 --up z --fit -o "$out/teapot1.bm3d" "$teapot"
"$mesh3d" --detail 2 --up z --fit --no-uvs -o "$out/teapot2.bm3d" "$teapot"
"$mesh3d" --detail 1 --no-normals --no-uvs --no-edges -o "$out/bare.bm3d" "$teapot"
cat > "$tmp/cube.obj" <<'OBJ'
v -1 -1 -1
v 1 -1 -1
v 1 1 -1
v -1 1 -1
v -1 -1 1
v 1 -1 1
v 1 1 1
v -1 1 1
vt 0 0
vt 1 0
vt 1 1
vt 0 1
o top
f 4/1 8/2 7/3 3/4
g sides
f 1/1 2/2 6/3 5/4
f 1 5 8 4
f 2 3 7 6
o bottom
f 1 4 3 2
f 5 6 7 8
OBJ
"$mesh3d" -o "$out/cube.bm3d" "$tmp/cube.obj"
"$mesh3d" --crease 0 --merge -o "$out/faceted.bm3d" "$tmp/cube.obj"
printf 'v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n' > "$tmp/one.obj"
"$mesh3d" --name a-name -o "$out/triangle.bm3d" "$tmp/one.obj"
