// Icons from the system's theme, marks written as a hand would, and confetti to end with.
float w = scene.width, h = scene.height;
string[] icons = {"folder", "text-x-generic", "audio-volume-high", "applications-games", "utilities-terminal",
                  "help-browser"};
SceneAnimation@[] shown;
for (uint i = 0; i < icons.length(); i++) {
        SceneIcon@ icon = SceneIcon(icons[i], 72).at(w * (i + 0.5) / icons.length(), h * 0.3);
        shown.insertLast(icon.grow().ease(Ease::Elastic));
}
scene.play(shown, 0.6, 0.1);
scene.pause();
SceneMark@ yes = SceneMark(Mark::Check, 90).at(w * 0.38, h * 0.66);
SceneMark@ no = SceneMark(Mark::Cross, 90).at(w * 0.62, h * 0.66);
scene.play(yes.write(), 0.5);
scene.play(no.write(), 0.5);
scene.pause();
scene.play(SceneConfetti().burst(), 3.5);
