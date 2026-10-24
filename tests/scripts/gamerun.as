// Screen.run(): draw() only after a change: a bool tick() returning false draws nothing new
// (on the host the window is a picture; run() keeps time all the same).
Screen w("Run", 32, 32);
class Quiet : Game {
        int ticks = 0, draws = 0;
        bool tick() {
                ticks++;
                if (ticks == 40) w.stop();
                return ticks % 10 == 0; // a change every tenth tick
        }
        void draw() { draws++; }
}
Quiet q;
w.run(q, 200);
println(q.ticks);
// The first picture, and one a change at most (catching up can fold changes into one draw).
println(q.draws >= 2 && q.draws <= 5);
class Busy : Game {
        int ticks = 0, draws = 0;
        void tick() {
                if (++ticks == 20) w.stop();
        }
        void draw() { draws++; }
}
Busy b;
w.run(b, 200);
println(b.ticks);
println(b.draws >= 4); // a void tick() may always have changed something
