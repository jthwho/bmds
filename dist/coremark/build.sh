# CoreMark: its core files and POSIX port as they are, ee_printf.c ours (run by bmds-dist; see
# dist/README.md). ITERATIONS=0: it times itself to run the 10 s a valid score needs. One context
# (MULTITHREAD=1): the score of a core.

mkdir -p "$DESTDIR/bin" "$DESTDIR/share/bench"
flags="-O2"
"$CC" $flags -I"$SRC/posix" -I"$SRC" -DHAS_PRINTF=0 -DITERATIONS=0 -DPERFORMANCE_RUN=1 \
        -DFLAGS_STR="\"$flags\"" \
        -DMEM_LOCATION="\"code in PSRAM, data on the heap\"" -include "$PKGDIR/ee_printf.h" \
        -o "$DESTDIR/bin/coremark" "$SRC/core_list_join.c" "$SRC/core_main.c" "$SRC/core_matrix.c" \
        "$SRC/core_state.c" "$SRC/core_util.c" "$SRC/posix/core_portme.c" "$PKGDIR/ee_printf.c"
cp "$PKGDIR/coremark.bench" "$DESTDIR/share/bench/coremark.bench"
