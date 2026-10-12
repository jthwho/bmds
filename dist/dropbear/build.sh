# Dropbear: configured out of tree with dist/dropbear/localoptions.h, built as one program
# (dropbearmulti) with a link for each of its programs (run by bmds-dist; see dist/README.md).

cp "$PKGDIR/localoptions.h" "$BUILD/localoptions.h"
cd "$BUILD"
# X25519 and Ed25519 from Monocypher (patch 0003), at -O2: TweetNaCl's were 11 to 37 times slower
# (devplan/network.md §26).
MONO="$ROOT/thirdparty/monocypher"
for f in monocypher monocypher-ed25519; do
        "$CC" -O2 -I"$MONO" -c "$MONO/$f.c" -o "$f.o"
done
"$AR" rcs libmonocypher.a monocypher.o monocypher-ed25519.o
# musl has a fork() that fails: Dropbear must take its no-fork paths. Its log goes to syslog(),
# so /dev/log and the system log (devplan/logging.md).
ac_cv_func_fork=no CPPFLAGS="-I$MONO" "$SRC/configure" --host=riscv32-unknown-elf CC="$CC" AR="$AR" RANLIB="$AR s" \
        --enable-static --enable-bundled-libtom --disable-harden --disable-zlib \
        --disable-lastlog --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx \
        --disable-loginfunc --disable-pututline --disable-pututxline > configure.log
$BMDS_MAKE PROGRAMS="dropbear dbclient dropbearkey dropbearconvert scp" MULTI=1 RANLIB="$AR s" \
        LIBS="$BUILD/libmonocypher.a"

mkdir -p "$DESTDIR/bin"
cp dropbearmulti "$DESTDIR/bin/dropbearmulti"
for link in dropbear dbclient ssh dropbearkey dropbearconvert scp; do
        ln -sf dropbearmulti "$DESTDIR/bin/$link"
done
