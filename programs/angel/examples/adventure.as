#!/rom/bin/angel
// A very small text adventure: rooms joined by exits, things to pick up, and a goal.
// Type "go north", "take key", "look", "inventory" or "quit".  Run it with:  angel adventure.as

class Room {
        string name;
        string description;
        Exits exits;
        string[] things;

        Room(const string &in n, const string &in d) {
                name = n;
                description = d;
        }
}

// Exits as two arrays side by side: the direction, and the room it leads to.
class Exits {
        string[] directions;
        int[] rooms;

        void add(const string &in direction, int room) {
                directions.insertLast(direction);
                rooms.insertLast(room);
        }
        int find(const string &in direction) const {
                for (uint i = 0; i < directions.length(); i++)
                        if (directions[i] == direction) return rooms[i];
                return -1;
        }
}

Room@[] rooms;
rooms.insertLast(Room("the hall", "A dusty hall. A door leads north; stairs go down."));
rooms.insertLast(Room("the library", "Shelves of old books. The hall is south."));
rooms.insertLast(Room("the cellar", "Cold and dark. Something glints in a corner. The stairs go up."));
rooms[0].exits.add("north", 1);
rooms[0].exits.add("down", 2);
rooms[1].exits.add("south", 0);
rooms[2].exits.add("up", 0);
rooms[2].things.insertLast("key");
rooms[1].things.insertLast("book");

int here = 0;
string[] carrying;

void look() {
        Room@ room = rooms[here];
        println("You are in " + room.name + ". " + room.description);
        if (room.things.length() > 0) println("You see: " + join(room.things, ", ") + ".");
}

println("Find the key and bring it back to the hall.");
look();
while (true) {
        string[] words = input("> ").trim().toLower().split();
        if (words.length() == 0) continue;
        string verb = words[0];
        string what = words.length() > 1 ? words[1] : "";
        if (verb == "quit") {
                break;
        } else if (verb == "look") {
                look();
        } else if (verb == "go") {
                int next = rooms[here].exits.find(what);
                if (next < 0) {
                        println("You can't go that way.");
                } else {
                        here = next;
                        look();
                }
        } else if (verb == "take") {
                int at = rooms[here].things.find(what);
                if (at < 0) {
                        println("There's no " + what + " here.");
                } else {
                        rooms[here].things.removeAt(at);
                        carrying.insertLast(what);
                        println("Taken: " + what + ".");
                }
        } else if (verb == "inventory") {
                println(carrying.length() == 0 ? "You carry nothing." : "You carry: " + join(carrying, ", ") + ".");
        } else {
                println("I don't know how to " + verb + ".");
        }
        if (here == 0 && carrying.find("key") >= 0) {
                println("You're back in the hall with the key. You win!");
                break;
        }
}
