# The font into share/soundfonts, its copyright and license beside the package's others.
mkdir -p "$DESTDIR/share/soundfonts" "$DESTDIR/share/licenses/soundfont-timgm6mb"
cp "$SRC/TimGM6mb.sf2" "$DESTDIR/share/soundfonts/"
cp "$PKGDIR/COPYRIGHT" "$PKGDIR/GPL-2" "$DESTDIR/share/licenses/soundfont-timgm6mb/"
