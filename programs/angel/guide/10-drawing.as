#!/rom/bin/angel
// Lesson 10: Drawing
//
// Screen opens a window to draw on. Places in it are counted in pixels (dots) from its top-left
// corner: x goes across to the right, y goes down. This one is 640 across and 480 down.

Screen w("My Picture", 640, 480);

// Colours have names (press F1 on Color to see them all), or are mixed from red, green and blue,
// each from 0 to 255.
uint skyBlue = rgb(120, 180, 255);
w.clear(skyBlue);

// The ground: a filled rectangle at x, y, so wide, so high.
w.fillRect(0, 380, 640, 100, Color::Green);

// The sun: a filled circle at its middle, with a radius.
w.fillCircle(540, 90, 50, Color::Yellow);
// Its rays: lines from one point to another, all round. cos and sin give the direction.
for (int i = 0; i < 12; i++) {
        float angle = i * PI / 6;
        w.line(540 + int(cos(angle) * 60), 90 + int(sin(angle) * 60), 540 + int(cos(angle) * 85),
               90 + int(sin(angle) * 85), Color::Orange);
}

// A house: a rectangle for the walls, a polygon for the roof (its corners' x and y in turn).
w.fillRect(120, 260, 200, 140, Color::Brown);
w.fillPolygon({100, 260, 220, 170, 340, 260}, Color::Red);
w.fillRect(195, 320, 50, 80, Color::DarkGray); // the door
w.fillRect(140, 290, 40, 40, Color::Sky);      // a window
w.rect(140, 290, 40, 40, Color::White);        // just its edge

// hsv mixes a colour by its hue (0 to 360 round the rainbow), saturation and brightness.
for (int i = 0; i < 7; i++) w.fillCircle(400 + i * 30, 330, 12, hsv(i * 50, 1, 1));

// Text: where, what, which colour, and how big (2 is twice the size).
w.text(20, 20, "My house", Color::White, 3);

// Or in one of the system's fonts: its family, a size in pixels, bold or italic.
Font sign("IBM Plex Sans 24 bold");
w.text(20, 76, "Welcome", Color::Navy, sign);

// The program ends here, but the window stays until you close it, so you can look at the
// picture. w.save("picture.png") would save it as a file.

// Try this:
//   1. Draw a tree: a brown rectangle for the trunk, a green circle for the leaves.
//   2. Make it night: a dark sky, a white moon, and stars with w.plot(x, y, Color::White) at
//      random(0, 639), random(0, 300).
//   3. Write your name in the sky, as big as fits.
