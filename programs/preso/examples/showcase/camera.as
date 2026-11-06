// The camera: everything is drawn through it. Zoom onto a detail, pan along, and back out.
float w = scene.width, h = scene.height;
SceneGrid@ grid = SceneGrid(w, h, 40).at(w / 2, h / 2);
scene.add(grid);
for (int i = 0; i < 9; i++) {
        SceneStar@ s = SceneStar(5, 14 + (i % 3) * 6).at(w * (0.1 + 0.1 * i), h * (0.25 + 0.06 * (i % 4)));
        s.fill = scene.theme.color(i % 2 == 0 ? "Script.Accent" : "Script.Highlight");
        scene.add(s);
}
SceneText@ fine = SceneText("the small print, up close", scene.theme.font("Small")).at(w * 0.78, h * 0.82);
SceneRect@ frame = SceneRect(fine.width + 16, fine.height + 10).at(fine.x, fine.y);
frame.cornerRadius = 6;
scene.add(frame);
scene.add(fine);
scene.pause();
scene.play(scene.zoomTo(frame, 20), 1.2);
scene.pause();
scene.play(scene.panTo(w * 0.3, h * 0.3), scene.zoomBy(0.6), 1.2);
scene.pause();
scene.play(scene.resetCamera(), 1);
