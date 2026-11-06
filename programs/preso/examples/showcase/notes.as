// Notes on a diagram: a brace under an array, a callout on one of its cells, and a label that
// stays with a dot as it moves.
float w = scene.width, h = scene.height;
SceneArray@ a = SceneArray({3, 1, 4, 1, 5, 9, 2, 6}, 64).at(w / 2, h * 0.42);
scene.play(a.fadeIn(), 0.5);
scene.pause();
SceneBrace@ brace = SceneBrace(a, Side::Below, "the first eight digits of pi");
scene.play(brace.fadeIn(), 0.5);
scene.pause();
SceneCallout@ note = SceneCallout("the largest", a.cell(5), Side::Above, 30);
scene.play(note.grow(), 0.5);
scene.pause();
SceneCircle@ dot = SceneCircle(10).at(w * 0.12, h * 0.9);
dot.fill = scene.theme.color("Script.Accent");
SceneLabel@ label = SceneLabel("a label follows", dot, Side::Right, 12);
scene.play(dot.fadeIn(), label.fadeIn(), 0.4);
scene.play(dot.move(w * 0.7, h * 0.9).ease(Ease::InOut), 2);
