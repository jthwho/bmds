# Newell's teapot

`fg_teapot_data.h` is freeglut's teapot data, from
<https://raw.githubusercontent.com/freeglut/freeglut/master/src/fg_teapot_data.h>
(downloaded 2026-11-07, SHA-256 `e6b5587e62acb59564c3801b18ed377c05e78083fbcc70b7bab2caeb4c573ed9`),
unchanged. Licence: MIT (the X11 licence, in the file's head). Its data is Martin Newell's teapot
(1975) as Philip Schneider sent it to Juhana Kouhia in 1990, scaled, its repeated control points
merged, a bottom added; the file carries that history and the original message.

`teapot.bpt` is made from it by `tools/teapot-bpt`: the 10 patches turned or mirrored into the
classic 32, every front outside, the knob's top one point again (freeglut spreads it by 0.002 for
GL's evaluators); and, for drawing without back faces or a depth buffer, the spout and handle cut
where they enter the body and Newell's gaps closed (the ring round the lid, the spout's end): 38. `bmds-mesh3d` (`sdk/tools/mesh3d`, `devplan/3d.md` §12d) tessellates it into a
`.bm3d` model:

    bmds-mesh3d --detail 3 --up z --fit --no-uvs -o teapot.bm3d thirdparty/teapot/teapot.bpt
