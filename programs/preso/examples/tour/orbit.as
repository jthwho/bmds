// A planet round a sun, a moon round the planet: it moves while the slide is shown.
float cx = region.width / 2, cy = region.height / 2;
Circle@ sun = Circle(40).at(cx, cy);
sun.fill = Color::Orange;
sun.fillOpacity = 1;
sun.color = Color::Orange;
Circle@ orbit = Circle(140).at(cx, cy);
orbit.color = theme.color("Script.Muted");
orbit.lineWidth = 2;
Circle@ planet = Circle(16);
planet.fill = theme.color("Script.Accent");
planet.fillOpacity = 1;
planet.color = planet.fill;
Circle@ moon = Circle(6);
moon.fill = Color::Gray;
moon.fillOpacity = 1;
moon.color = moon.fill;
add(orbit);
add(sun);
add(planet);
add(moon);
float a = 0;
while (true) {
        a += 0.02;
        float px = cx + 140 * cos(a), py = cy + 140 * sin(a);
        planet.x = px;
        planet.y = py;
        moon.x = px + 32 * cos(a * 5);
        moon.y = py + 32 * sin(a * 5);
        wait(1.0 / 60);
}
