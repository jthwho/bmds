// Code a few lines at a time, the rest dimmed; then changed into a shorter version, the lines it
// keeps sliding to their places.
string before = """int sum(int[] v) {
    int total = 0;
    for (uint i = 0; i < v.length(); i++)
        total += v[i];
    return total;
}""";
string after = """int sum(int[] v) {
    int total = 0;
    foreach (int x : v)
        total += x;
    return total;
}""";
SceneCode@ code = SceneCode(before, "angel").at(scene.width / 2, scene.height / 2);
scene.play(code.fadeIn(), 0.5);
scene.pause();
scene.play(code.highlight({2}), 0.4);
scene.pause();
scene.play(code.highlight({3, 4}), 0.4);
scene.pause();
scene.play(code.highlight({5}), 0.4);
scene.pause();
scene.play(code.highlight({}), 0.4);
scene.play(code.transformTo(after), 1.2);
scene.pause();
scene.play(code.highlight({3, 4}), 0.4);
