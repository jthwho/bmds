// Something to play with (Tab or a click enters the region): the slider sets the wave's
// frequency, the toggle shows its cosine too.
float w = scene.width, h = scene.height;
SceneAxes@ axes = SceneAxes(640, 220, 0, 6.3, -1.3, 1.3).at(w / 2, h * 0.38);
scene.add(axes);
SceneSlider@ frequency = SceneSlider(1, 5, 1, 320).at(w * 0.35, h * 0.86);
SceneToggle@ both = SceneToggle("cosine too").at(w * 0.75, h * 0.86);
SceneLabel@ label = SceneLabel("frequency", frequency, Side::Above);
SceneCurve@ wave = null, other = null;

void redraw() {
        if (wave !is null) scene.remove(wave);
        if (other !is null) scene.remove(other);
        @wave = axes.plot(function(x) { return sin(x * frequency.value); });
        scene.add(wave);
        @other = null;
        if (both.on) {
                @other = axes.plot(function(x) { return cos(x * frequency.value); }, scene.theme.color("Script.Highlight"),
                                   100);
                scene.add(other);
        }
}

scene.add(frequency);
scene.add(both);
scene.add(label);
redraw();
frequency.onChange(function() { redraw(); });
both.onChange(function() { redraw(); });
