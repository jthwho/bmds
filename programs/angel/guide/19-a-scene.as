#!/rom/bin/angel
// Lesson 19: A scene, played a step at a time
//
// Lesson 11 drew every frame itself. A scene works the other way round: you make objects (a
// SceneRect, a SceneText, a SceneCircle...), put them in the scene, and play animations on
// them; the drawing is done for you, smoothly, and you say only what changes. scene.pause()
// waits for Space (or a click): a step, as in a talk. Left goes back a step.
//
// The same script works in a Preso slide: ![script w:900 h:380](19-a-scene.as) in a deck plays
// it there, Next for Space. Run it here with F5; close the window to stop.

float w = scene.width, h = scene.height;

// Objects: made, placed with at(x, y) (their middle), coloured from the scene's theme.
SceneText@ title = SceneText("Sorting three cards", scene.theme.font("H2")).at(w / 2, 60);
scene.add(title);
scene.play(title.write(), 1); // write() brings text in a letter at a time

int[] values = {3, 1, 2};
SceneRect@[] cards;
SceneText@[] labels;
for (uint i = 0; i < values.length(); i++) {
        float x = w / 2 + (float(i) - 1) * 160;
        SceneRect@ card = SceneRect(120, 160).at(x, h / 2);
        card.fill = scene.theme.color("Script.Fill");
        SceneText@ label = SceneText(values[i]).at(x, h / 2);
        cards.insertLast(card);
        labels.insertLast(label);
        scene.add(card);
        scene.add(label);
}
scene.play(cards[0].fadeIn(), cards[1].fadeIn(), cards[2].fadeIn(), 0.5);
scene.pause();

// Animations played together: two cards (and their numbers) change places.
void swap(uint a, uint b) {
        float xa = cards[a].x, xb = cards[b].x;
        scene.play(cards[a].move(xb, h / 2), labels[a].move(xb, h / 2), cards[b].move(xa, h / 2),
                   labels[b].move(xa, h / 2), 0.6);
        SceneRect@ c = cards[a];
        @cards[a] = cards[b];
        @cards[b] = c;
        SceneText@ t = labels[a];
        @labels[a] = labels[b];
        @labels[b] = t;
        int v = values[a];
        values[a] = values[b];
        values[b] = v;
}

// A bubble sort, each swap a step you see.
for (uint pass = 0; pass < values.length(); pass++) {
        for (uint i = 0; i + 1 < values.length(); i++) {
                if (values[i] <= values[i + 1]) continue;
                swap(i, i + 1);
                scene.pause();
        }
}
title.text = "Sorted";
scene.play(cards[0].fillTo(scene.theme.color("Script.Highlight")),
           cards[1].fillTo(scene.theme.color("Script.Highlight")),
           cards[2].fillTo(scene.theme.color("Script.Highlight")), 0.5);
println("sorted: " + values[0] + " " + values[1] + " " + values[2]);
