// What any object can do: grow, move with an ease, turn and change colour, pulse, wiggle, turn
// into another shape; and many at once, each a little after the one before.
float w = scene.width, h = scene.height;
SceneRect@ box = SceneRect(130, 84).at(w * 0.2, h * 0.42);
box.cornerRadius = 10;
box.fill = scene.theme.color("Script.Fill");
SceneText@ name = SceneText("box").at(w * 0.2, h * 0.42);
name.follow(function(SceneObject@ self) {
        self.x = box.x;
        self.y = box.y;
});
scene.play(box.grow(), 0.5);
scene.add(name); // after the box: drawn over it
scene.pause();
scene.play(box.move(w * 0.5, h * 0.42).ease(Ease::Bounce), 1);
scene.pause();
scene.play(box.rotateBy(90), box.fillTo(scene.theme.color("Script.Highlight")), 0.8);
scene.pause();
scene.play(box.indicate(), 0.8);
scene.play(box.wiggle(), 0.8);
scene.pause();
SceneStar@ star = SceneStar(5, 60).at(w * 0.8, h * 0.42);
star.fill = scene.theme.color("Script.Accent");
scene.play(box.transform(star), name.fadeOut(), 1); // its name was a box's
scene.pause();
SceneAnimation@[] dots;
for (int i = 0; i < 14; i++) {
        SceneCircle@ d = SceneCircle(13).at(w * (i + 0.5) / 14, h * 0.85);
        d.fill = scene.theme.color("Script.Accent");
        dots.insertLast(d.grow().ease(Ease::Elastic));
}
scene.play(dots, 0.7, 0.07);
