// Functions plotted on axes, then a dot riding the curve with its name beside it.
float w = scene.width, h = scene.height;
SceneAxes@ axes = SceneAxes(760, 300, -6.3, 6.3, -1.5, 1.5).at(w / 2, h / 2);
scene.play(axes.fadeIn(), 0.5);
SceneCurve@ wave = axes.plot(function(x) { return sin(x); });
SceneCurve@ other = axes.plot(function(x) { return cos(x); }, scene.theme.color("Script.Highlight"), 160);
scene.play(wave.write(), 1.2);
scene.pause();
scene.play(other.write(), 1.2);
scene.pause();
SceneCircle@ dot = SceneCircle(9).at(axes.toX(-6.3), axes.toY(sin(-6.3)));
dot.fill = scene.theme.color("Script.Accent");
SceneLabel@ name = SceneLabel("sin x", dot, Side::Above);
scene.play(dot.fadeIn(), name.fadeIn(), 0.3);
scene.play(dot.moveAlong(wave).ease(Ease::Linear), 4);
