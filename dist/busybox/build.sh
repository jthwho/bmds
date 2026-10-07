# BusyBox: built out of tree with dist/busybox/config, installed as
# bin/busybox and one link per applet (run by bmds-dist; see dist/README.md).

cp "$PKGDIR/config" "$BUILD/.config"
cd "$SRC"
# A config from an older recipe: options it doesn't know stay off.
yes "" | make -s O="$BUILD" oldconfig > /dev/null
$BMDS_MAKE busybox busybox.links O="$BUILD" CC="$CC" HOSTCC="$HOSTCC" AR="$AR" STRIP="$STRIP"

mkdir -p "$DESTDIR/bin"
cp "$BUILD/busybox_unstripped" "$DESTDIR/bin/busybox"
# busybox.links lists /bin/ls, /usr/bin/awk ...: all go next to it.
while read -r path; do
        applet="${path##*/}"
        [ "$applet" = busybox ] || ln -sf busybox "$DESTDIR/bin/$applet"
done < "$BUILD/busybox.links"
# Launcher entries for the panel (devplan/gui.md G11).
mkdir -p "$DESTDIR/share/applications"
cp "$PKGDIR"/applications/*.desktop "$DESTDIR/share/applications/"
