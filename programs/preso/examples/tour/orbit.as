// A planet round a sun, a moon round the planet: it moves while the slide is shown.
float cx = scene.width / 2, cy = scene.height / 2;
SceneCircle@ sun = SceneCircle(40).at(cx, cy);
sun.fill = Color::Orange;
sun.fillOpacity = 1;
sun.color = Color::Orange;
SceneCircle@ orbit = SceneCircle(140).at(cx, cy);
orbit.color = scene.theme.color("Script.Muted");
orbit.lineWidth = 2;
SceneCircle@ planet = SceneCircle(16);
planet.fill = scene.theme.color("Script.Accent");
planet.fillOpacity = 1;
planet.color = planet.fill;
SceneCircle@ moon = SceneCircle(6);
moon.fill = Color::Gray;
moon.fillOpacity = 1;
moon.color = moon.fill;
scene.add(orbit);
scene.add(sun);
scene.add(planet);
scene.add(moon);
float a = 0;
while (true) {
        a += 0.02;
        float px = cx + 140 * cos(a), py = cy + 140 * sin(a);
        planet.x = px;
        planet.y = py;
        moon.x = px + 32 * cos(a * 5);
        moon.y = py + 32 * sin(a * 5);
        scene.wait(1.0 / 60);
}
