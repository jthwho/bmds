// The scene's shapes, a row a step: arrows with heads of every kind, shapes, then a plot and a
// number counting. Each runs alone too: angel tour/shapes.as.
float w = scene.width, h = scene.height;

// Arrows: a head at the end (and here a tail too), straight, bent and dashed.
ArrowHead[] heads = {ArrowHead::Triangle, ArrowHead::Stealth, ArrowHead::Open, ArrowHead::Circle,
                     ArrowHead::Diamond, ArrowHead::Bar};
SceneAnimation@[] drawn;
for (uint i = 0; i < heads.length(); i++) {
        float x = w * (i + 0.5) / heads.length();
        SceneArrow@ a = SceneArrow(x - 50, 60, x + 50, 60);
        a.head = heads[i];
        a.tail = i % 2 == 0 ? ArrowHead::None : heads[i];
        a.bend = i % 3 == 2 ? 50 : 0;
        a.dash = i == 4 ? 10 : 0;
        a.color = scene.theme.color("Script.Line");
        drawn.insertLast(a.write());
}
scene.play(drawn, 0.6, 0.1);
scene.pause();

// Shapes: grown in one after another.
float y = h / 2 - 10;
SceneObject@[] shapes = {
        SceneEllipse(110, 70), SceneRegular(6, 45), SceneStar(5, 48), SceneSector(48, -90, 250),
        SceneCurve({-50, 20, -25, -30, 0, 20, 25, -30, 50, 20}),
};
SceneAnimation@[] grown;
for (uint i = 0; i < shapes.length(); i++) {
        shapes[i].at(w * (i + 0.5) / shapes.length(), y);
        shapes[i].fill = scene.theme.color(i % 2 == 0 ? "Script.Fill" : "Script.Highlight");
        grown.insertLast(shapes[i].grow());
}
scene.play(grown, 0.5, 0.12);
scene.pause();

// A plot on axes, and a number counting to its last value.
SceneAxes@ axes = SceneAxes(w * 0.55, 110, 0, 6.3, -1.2, 1.2).at(w * 0.33, h - 75);
scene.play(axes.fadeIn(), 0.4);
SceneCurve@ wave = axes.plot(function(x) { return sin(x); });
SceneNumber@ n = SceneNumber(0, 2, scene.theme.font("H2")).at(w * 0.8, h - 75);
scene.add(n);
scene.play(wave.write(), n.countTo(3.14), 1.5);
scene.play(n.indicate(), 0.6);
