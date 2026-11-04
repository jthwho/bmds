// A diagram built a step at a time: each pause() waits for Next.
float w = region.width, h = region.height;
string[] names = {"Markdown", "Layout", "Slides"};
Shape@[] boxes;
for (uint i = 0; i < names.length(); i++) {
        float x = w * (i + 0.5) / names.length();
        Rect@ r = Rect(200, 90).at(x, h / 2);
        r.color = theme.color("Script.Line");
        r.fill = theme.color("Script.Fill");
        r.fillOpacity = 1;
        Text@ t = Text(names[i]).at(x, h / 2);
        if (i > 0) pause();
        add(r);
        add(t);
        play(fadeIn(r), fadeIn(t), 0.4);
        if (i > 0) {
                Arrow@ arrow = Arrow(w * (i - 0.5) / names.length() + 105, h / 2, x - 105, h / 2);
                arrow.color = theme.color("Script.Accent");
                add(arrow);
                play(write(arrow), 0.4);
        }
}
