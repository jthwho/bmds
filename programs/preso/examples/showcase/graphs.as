// A graph: laid out as a tree and walked breadth first; a path lit up; then laid out round a
// circle, its edges following its nodes.
SceneGraph@ g = SceneGraph().at(scene.width / 2, scene.height / 2 - 10);
string[] names = {"A", "B", "C", "D", "E", "F", "G"};
for (uint i = 0; i < names.length(); i++) g.addNode(names[i]);
g.addEdge(0, 1);
g.addEdge(0, 2);
g.addEdge(1, 3);
g.addEdge(1, 4);
g.addEdge(2, 5);
g.addEdge(2, 6);
g.layoutTree(0, 100, 90);
scene.play(g.fadeIn(), 0.6);
scene.pause();
for (uint i = 0; i < g.nodes; i++) scene.play(g.visit(i), 0.25);
scene.pause();
SceneAnimation@[] back;
for (uint i = 0; i < g.nodes; i++) back.insertLast(g.unvisit(i));
scene.play(back, 0.3);
scene.play(g.highlightPath({0, 2, 6}), 1.2, 0.25);
scene.pause();
float[] fromX, fromY;
for (uint i = 0; i < g.nodes; i++) {
        fromX.insertLast(g.node(i).x);
        fromY.insertLast(g.node(i).y);
}
g.layoutCircle(150);
SceneAnimation@[] moves;
for (uint i = 0; i < g.nodes; i++) {
        float x = g.node(i).x, y = g.node(i).y;
        g.node(i).x = fromX[i];
        g.node(i).y = fromY[i];
        moves.insertLast(g.node(i).move(x, y));
}
scene.play(moves, 1.2);
