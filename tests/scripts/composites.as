// Arrays, tables, graphs, code, notes, updaters, paths, charts and the camera (devplan/preso.md
// §8e), checked as numbers: what each holds after its animations, and where things end up.
SceneArray@ a = SceneArray({5, 2, 8});
scene.add(a);
scene.play(a.swap(0, 1), a.pointTo("i", 2), 0.5);
println("array: " + a.value(0) + " " + a.value(1) + " " + a.value(2) + ", " + a.count + " cells");
scene.play(a.set(2, "x"), 0.3);
println("set: " + a.value(2));
string[] words = {"to", "be"};
println("words: " + SceneArray(words).value(1));

SceneTable@ t = SceneTable(2, 3);
t.set(1, 2, 3.5);
t.header = true;
println("table: " + t.rows + " x " + t.columns + ", (1, 2) = " + t.get(1, 2) + ", header " + t.header);

SceneGraph@ g = SceneGraph();
for (int i = 0; i < 4; i++) g.addNode(i);
g.addEdge(0, 1);
g.addEdge(0, 2);
g.addEdge(2, 3, false);
g.layoutTree(0, 100, 80);
println("tree: root at " + g.node(0).x + ", " + g.node(0).y + "; leaf 3 at " + g.node(3).x + ", " + g.node(3).y);
println("graph: " + g.nodes + " nodes, " + g.edges + " edges");
g.layoutRow(100);
println("row: " + g.node(0).x + " .. " + g.node(3).x);
SceneAnimation@[] path = g.highlightPath({0, 2, 3});
println("path: " + path.length() + " animations");

SceneCode@ code = SceneCode("a\nb\nc", "");
scene.add(code);
scene.play(code.highlight({2}), 0.2);
println("code: " + code.lineCount + " lines, dim " + code.dim);
scene.play(code.transformTo("a\nc\nd\ne"), 0.5);
println("transformed: " + code.lineCount + " lines, objects in the scene " + (scene.has(code) ? "the code" : "?"));

SceneRect@ box = SceneRect(100, 40).at(300, 200);
SceneLabel@ label = SceneLabel("hello", box, Side::Below, 10);
scene.add(box);
scene.add(label);
scene.play(box.move(500, 200), 0.5);
scene.wait(0.1);
println("label: text '" + label.text + "', under the box: " + (label.top >= box.bottom ? "yes" : "no"));
SceneCallout@ callout = SceneCallout("look", a.cell(1), Side::Above);
println("callout: '" + callout.text + "' above the cell, its pointer to it: " + (callout.top < a.top && callout.bottom >= a.top - 4 ? "yes" : "no"));

SceneNumber@ n = SceneNumber(0);
n.follow(function(SceneObject@ self) { cast<SceneNumber>(self).value = box.x; });
scene.add(n);
scene.wait(0.1);
SceneCircle@ dot = SceneCircle(5);
SceneLine@ track = SceneLine(0, 0, 200, 100);
scene.add(dot);
scene.play(dot.moveAlong(track), 1);
println("along: " + dot.x + ", " + dot.y);

SceneBarChart@ bars = SceneBarChart({1, 2, 3}, {"a", "b", "c"});
scene.add(bars);
scene.play(bars.build(), 0.5);
scene.play(bars.setValue(1, 2.5), 0.5);
println("bars: " + bars.count + " values, the second " + bars.value(1));
ScenePieChart@ pie = ScenePieChart({1, 1, 2}, {"x", "y", "z"});
scene.play(pie.setValue(2, 4), 0.2);
println("pie: the third " + pie.value(2));

scene.play(scene.zoomTo(box, 0), 0.5);
println("camera: at " + scene.cameraX + ", " + scene.cameraY + " zoom " + (scene.zoom > 1 ? "in" : "out"));
scene.play(scene.resetCamera(), 0.5);
println("reset: zoom " + scene.zoom);
SceneConfetti@ c = SceneConfetti();
scene.play(c.burst(), 0.5);
println("confetti gone: " + !scene.has(c));
println("time: " + millis());
