// A diagram built a step at a time: each scene.pause() waits for Next.
float w = scene.width, h = scene.height;
string[] names = {"Markdown", "Layout", "Slides"};
SceneRect@ last = null;
for (uint i = 0; i < names.length(); i++) {
        float x = w * (i + 0.5) / names.length();
        SceneRect@ r = SceneRect(200, 90).at(x, h / 2);
        r.color = scene.theme.color("Script.Line");
        r.fill = scene.theme.color("Script.Fill");
        r.cornerRadius = 12;
        SceneText@ t = SceneText(names[i]).at(x, h / 2);
        if (i > 0) scene.pause();
        scene.add(r);
        scene.add(t);
        scene.play(r.fadeIn(), t.fadeIn(), 0.4);
        if (last !is null) {
                // From the last box to this one, its ends on their edges.
                SceneArrow@ arrow = SceneArrow().between(last, r, 6);
                arrow.color = scene.theme.color("Script.Accent");
                scene.add(arrow);
                scene.play(arrow.write(), 0.4);
        }
        @last = r;
}
