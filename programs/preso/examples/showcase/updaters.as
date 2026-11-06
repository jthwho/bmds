// Updaters keep things in step before each frame: a number shows where the dot is, and a line
// ties them together. moveAlong() takes the dot round the dashed track; the arrowhead after it
// turns to face the way it goes.
float w = scene.width, h = scene.height;
SceneCurve@ track = SceneCurve({w * 0.1, h * 0.55, w * 0.3, h * 0.18, w * 0.5, h * 0.62, w * 0.7, h * 0.2,
                                w * 0.9, h * 0.55});
track.color = scene.theme.color("Script.Muted");
track.dash = 10;
SceneCircle@ dot = SceneCircle(14).at(w * 0.1, h * 0.55);
dot.fill = scene.theme.color("Script.Accent");
SceneNumber@ readout = SceneNumber(0, 0, scene.theme.font("H2")).at(w / 2, h * 0.88);
SceneLine@ tie = SceneLine();
tie.dash = 4;
tie.color = scene.theme.color("Script.Muted");
readout.follow(function(SceneObject@ self) { cast<SceneNumber>(self).value = dot.x; });
tie.follow(function(SceneObject@ self) {
        SceneLine@ l = cast<SceneLine>(self);
        l.startX = dot.x;
        l.startY = dot.y;
        l.endX = readout.x;
        l.endY = readout.top;
});
scene.play(track.write(), 1);
scene.add(tie);
scene.add(dot);
scene.add(readout);
scene.pause();
scene.play(dot.moveAlong(track), 3);
scene.pause();
ScenePolygon@ arrow = ScenePolygon({-12, -9, 14, 0, -12, 9});
arrow.fill = scene.theme.color("Script.Highlight");
arrow.at(w * 0.1, h * 0.55);
scene.add(arrow);
scene.play(arrow.moveAlong(track, true), 3);
