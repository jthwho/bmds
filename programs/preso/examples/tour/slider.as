// A slider and what it sets: Tab or a click enters the region, the arrows or the mouse move it.
float w = region.width, h = region.height;
Circle@ c = Circle(60).at(w / 2, h / 2 - 40);
c.fill = theme.color("Script.Accent");
c.fillOpacity = 1;
c.color = c.fill;
add(c);
Slider@ size = Slider(20, 140, 60, 400).at(w / 2, h - 50);
add(size);
size.onChange(function() { c.radius = size.value; });
