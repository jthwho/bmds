# i2c-tools, built with the SDK's compiler directly (upstream's Makefile builds a shared libi2c
# too): libi2c and the tools' common files into one archive, then the tools as one program with
# a link for each (run by bmds-dist; see dist/README.md).

cd "$BUILD"
flags="-O2 -I$SRC/include -I$SRC"
objs=()
for f in "$SRC/lib/smbus.c" "$SRC/tools/i2cbusses.c" "$SRC/tools/util.c"; do
        o="$(basename "$f" .c).o"
        "$CC" $flags -c "$f" -o "$o"
        objs+=("$o")
done
"$AR" rcs libi2ctools.a "${objs[@]}"

tools="i2cdetect i2cget i2cset i2cdump i2ctransfer"
mains=()
for tool in $tools; do
        "$CC" $flags -Dmain="${tool}_main" -c "$SRC/tools/$tool.c" -o "$tool.o"
        mains+=("$tool.o")
done
mkdir -p "$DESTDIR/bin"
"$CC" $flags -o "$DESTDIR/bin/i2cmulti" "$PKGDIR/i2cmulti.c" "${mains[@]}" libi2ctools.a
for tool in $tools; do
        ln -sf i2cmulti "$DESTDIR/bin/$tool"
done
