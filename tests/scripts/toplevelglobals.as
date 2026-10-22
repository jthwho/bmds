// Globals declared among the top-level statements are initialized where they stand, with an
// initializer or without: an object without one is constructed after the statements above it
// (the top-level statements patch, thirdparty/angelscript/README.md).
int[] made;
class Pong {
        int seen;
        Pong() { seen = made.length(); }
}
made.insertLast(1);
made.insertLast(2);
Pong p;            // constructed here: it sees the two
println(p.seen);
int n;             // a primitive without an initializer: 0
string s;          // an object: empty
println(n + " [" + s + "]");
Pong q = Pong();   // with an initializer, as before
made.insertLast(3);
Pong r;
println(q.seen + " " + r.seen);
