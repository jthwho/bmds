// The scene's shapes and layout (devplan/preso.md §8d), checked as numbers: copies are new objects,
// nextTo and arrange place by boxes, between puts a line's ends on two boxes, numbers count,
// plots follow their function, and the scene's order and contents change as asked.
SceneRect@ a = SceneRect(100, 60).at(100, 100);
SceneRect@ b = a.copy().at(400, 100);
println("copy: " + (a is b ? "same" : "new") + ", " + b.width + " x " + b.height);
SceneCircle@ c = SceneCircle(20).nextTo(a, Side::Right, 10);
println("nextTo: left " + c.left + " is a's right " + a.right + " + 10, y " + c.y);
SceneArrow@ link = SceneArrow().between(a, b);
println("between: " + link.startX + ", " + link.startY + " to " + link.endX + ", " + link.endY);
link.endY = 200;
println("end moved: " + link.endY + ", start kept " + link.startX + ", " + link.startY);
println("heads: " + link.head + " " + link.tail);
SceneGroup@ g = SceneGroup().at(0, 0);
for (int i = 0; i < 3; i++) g.add(SceneRect(20 + i * 10, 20));
g.arrange(Side::Right, 5);
SceneGroup@ again = g.copy();
println("arrange: copied group has " + again.count + ", its box " + g.left + " to " + g.right);
SceneNumber@ n = SceneNumber(0, 1);
scene.add(a);
scene.add(b);
scene.add(n);
scene.play(n.countTo(12.5), 1);
println("counted to " + n.value + " (" + n.decimals + " decimal)");
SceneAxes@ ax = SceneAxes(200, 100, 0, 4, 0, 2).at(300, 300);
println("axes: x 0 at " + ax.toX(0) + ", 4 at " + ax.toX(4) + "; y 2 at " + ax.toY(2));
SceneCurve@ line = ax.plot(function(x) { return x / 2; }, Color::Red, 5);
println("plot: " + line.count + " points, closed " + line.closed);
scene.toBack(b);
println("has a " + scene.has(a) + ", has the plot " + scene.has(line));
scene.play(a.indicate(), b.wiggle(), 0.5);
println("after indicate: scale " + a.scale + ", rotation " + b.rotation);
scene.play({a.fadeTo(0.5), b.rotateBy(90), n.scaleBy(2)}, 0.5, 0.25);
println("lagged: opacity " + a.opacity + ", rotation " + b.rotation + ", scale " + n.scale + " at " + millis());
scene.removeAll();
println("removed all: has a " + scene.has(a));
