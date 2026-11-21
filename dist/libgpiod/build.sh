# libgpiod's tools, built with the SDK's compiler directly (upstream builds with meson): the
# library, libgpiotools and tools-common into one archive, then the tools as one program (run by
# bmds-dist; see dist/README.md). gpioset's interactive mode needs libedit: left out, as
# upstream does without it. Patch 0001: the chip check without /sys.

cd "$BUILD"
flags="-O2 -D_GNU_SOURCE -DGPIOD_VERSION_STR=\"$VERSION\" -DGPIOD_API_VERSION_STR=\"2.2.1\""
inc="-I$SRC/include -I$SRC/lib -I$SRC/tools"
objs=()
for f in "$SRC"/lib/*.c "$SRC/tools/gpiotools.c" "$SRC/tools/tools-common.c"; do
        o="$(basename "$f" .c).o"
        "$CC" $flags $inc -c "$f" -o "$o"
        objs+=("$o")
done
"$AR" rcs libgpiod.a "${objs[@]}"

# One program, a link for each tool (as Dropbear's): the library and the C library once rather
# than six times (about 230 KB each apart).
tools="gpiodetect gpioinfo gpioget gpioset gpiomon gpionotify"
mains=()
for tool in $tools; do
        "$CC" $flags $inc -Dmain="${tool}_main" -c "$SRC/tools/$tool.c" -o "$tool.o"
        mains+=("$tool.o")
done
mkdir -p "$DESTDIR/bin"
"$CC" $flags -o "$DESTDIR/bin/gpiodmulti" "$PKGDIR/gpiodmulti.c" "${mains[@]}" libgpiod.a
for tool in $tools; do
        ln -sf gpiodmulti "$DESTDIR/bin/$tool"
done
