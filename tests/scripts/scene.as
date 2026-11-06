// A scene under angel (devplan/preso.md §8c): its first scene call hands the script to the stage,
// which runs it again from its start; with no screen it plays each beat to its end at once.
// What was printed before that call isn't printed again, and the scene's clock is its own.
println("set up: " + scene.width + " x " + scene.height);
SceneRect@ box = SceneRect(100, 50).at(200, 150);
box.fill = scene.theme.color("Script.Fill");
SceneText@ label = SceneText("Hello").at(200, 150);
SceneObject@[] all = {box, label};
scene.add(box);
scene.add(label);
println("added " + all.length());
scene.play(box.fadeIn(), label.write(), 0.5);
println("faded in at " + millis());
scene.pause();
scene.play(box.move(400, 300), box.rotateTo(90).ease(Ease::Bounce), 1);
println("moved to " + box.x + ", " + box.y + " turned " + box.rotation + " at " + millis());
SceneRect@ back = cast<SceneRect>(all[0]);
println(back is box);
print(readText("scene.txt"));
