// An array sorted by bubble sort, a pass a step: j points at the pair compared, and a swap is
// shown as it happens. Each pass leaves its largest at the end, marked.
int[] v = {5, 2, 8, 1, 9, 3};
uint done = scene.theme.color("Script.Accent"); // a cell in its place
SceneArray@ a = SceneArray(v, 76).at(scene.width / 2, scene.height / 2 - 30);
scene.play(a.fadeIn(), 0.5);
for (uint pass = 0; pass + 1 < v.length(); pass++) {
        scene.pause();
        for (uint j = 0; j + 1 < v.length() - pass; j++) {
                scene.play(a.pointTo("j", j), a.highlight(j), a.highlight(j + 1), 0.3);
                if (v[j] > v[j + 1]) {
                        int t = v[j];
                        v[j] = v[j + 1];
                        v[j + 1] = t;
                        scene.play(a.swap(j, j + 1), 0.5);
                }
                scene.play(a.unhighlight(j), a.unhighlight(j + 1), 0.15);
        }
        scene.play(a.highlight(v.length() - 1 - pass, done), 0.2);
}
scene.play(a.highlight(0, done), 0.2);
