# Dropbear: configured out of tree with dist/dropbear/localoptions.h, built as one program
# (dropbearmulti) with a link for each of its programs (run by bmds-dist; see dist/README.md).

cp "$PKGDIR/localoptions.h" "$BUILD/localoptions.h"
cd "$BUILD"
# musl has a fork() that fails: Dropbear must take its no-fork paths. Its log goes to syslog(),
# so /dev/log and the system log (devplan/logging.md).
ac_cv_func_fork=no "$SRC/configure" --host=riscv32-unknown-elf CC="$CC" AR="$AR" RANLIB="$AR s" \
        --enable-static --enable-bundled-libtom --disable-harden --disable-zlib \
        --disable-lastlog --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx \
        --disable-loginfunc --disable-pututline --disable-pututxline > configure.log
$BMDS_MAKE PROGRAMS="dropbear dbclient dropbearkey dropbearconvert scp" MULTI=1 RANLIB="$AR s"

mkdir -p "$DESTDIR/bin"
cp dropbearmulti "$DESTDIR/bin/dropbearmulti"
for link in dropbear dbclient ssh dropbearkey dropbearconvert scp; do
        ln -sf dropbearmulti "$DESTDIR/bin/$link"
done
