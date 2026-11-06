// A table filled a cell at a time: each Fibonacci number is the two before it added.
SceneTable@ t = SceneTable(2, 10, 76, 52).at(scene.width / 2, scene.height / 2);
t.header = true;
for (uint c = 0; c < 10; c++) t.set(0, c, c);
t.set(1, 0, 0);
t.set(1, 1, 1);
scene.play(t.fadeIn(), 0.5);
scene.pause();
int[] f = {0, 1};
for (uint c = 2; c < 10; c++) {
        f.insertLast(f[c - 1] + f[c - 2]);
        t.set(1, c, f[c]);
        t.text(1, c).opacity = 0;
        scene.play(t.highlight(1, c - 1), t.highlight(1, c - 2), 0.25);
        scene.play(t.text(1, c).fadeTo(1), t.cell(1, c).fillTo(scene.theme.color("Script.Fill")), 0.25);
        scene.play(t.unhighlightAll(), 0.15);
}
scene.pause();
scene.play(t.highlightColumn(9), 0.5);
