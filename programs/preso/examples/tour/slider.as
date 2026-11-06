// A slider and what it sets: Tab or a click enters the region, the arrows or the mouse move it.
float w = scene.width, h = scene.height;
SceneCircle@ c = SceneCircle(60).at(w / 2, h / 2 - 40);
c.fill = scene.theme.color("Script.Accent");
c.fillOpacity = 1;
c.color = c.fill;
scene.add(c);
SceneSlider@ size = SceneSlider(20, 140, 60, 400).at(w / 2, h - 50);
scene.add(size);
size.onChange(function() { c.radius = size.value; });
