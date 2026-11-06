// Every shape, a row a step: closed shapes (filled or not), then open ones and marks.
float w = scene.width, h = scene.height;
SceneRect@ rounded = SceneRect(110, 70);
rounded.cornerRadius = 14;
SceneObject@[] closed = {rounded, SceneCircle(42), SceneEllipse(120, 70), SceneRegular(6, 44), SceneStar(5, 46),
                         SceneSector(46, -90, 270)};
SceneAnimation@[] grown;
for (uint i = 0; i < closed.length(); i++) {
        closed[i].at(w * (i + 0.5) / closed.length(), h * 0.3);
        closed[i].fill = scene.theme.color(i % 2 == 0 ? "Script.Fill" : "Script.Highlight");
        grown.insertLast(closed[i].grow());
}
scene.play(grown, 0.5, 0.1);
scene.pause();

SceneArc@ arc = SceneArc(42, 180, 270);
arc.head = ArrowHead::Triangle;
SceneArrow@ both = SceneArrow(-55, 0, 55, 0);
both.tail = ArrowHead::Stealth;
both.head = ArrowHead::Stealth;
SceneArrow@ bent = SceneArrow(-55, 15, 55, 15);
bent.bend = 70;
bent.head = ArrowHead::Open;
SceneLine@ dashed = SceneLine(-55, 0, 55, 0);
dashed.dash = 10;
dashed.tail = ArrowHead::Circle;
dashed.head = ArrowHead::Diamond;
SceneObject@[] open = {arc, SceneCurve({-60, 20, -30, -30, 0, 20, 30, -30, 60, 20}), both, bent, dashed,
                       SceneMark(Mark::Check, 64)};
SceneAnimation@[] written;
for (uint i = 0; i < open.length(); i++) {
        open[i].at(w * (i + 0.5) / open.length(), h * 0.72);
        written.insertLast(open[i].write());
}
scene.play(written, 0.6, 0.12);
