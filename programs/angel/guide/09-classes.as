#!/rom/bin/angel
// Lesson 9: Classes
//
// A class is a new type of your own: a few variables that belong together, and the functions
// that work on them. A game's player has a name, a score and lives; a class keeps them in one
// thing, an object.

class Pet {
        string name;   // the variables each Pet has
        string kind;
        int hunger = 5;

        // A constructor: what happens when a Pet is made. This one takes nothing, and makes a
        // pet with no name yet (a list of pets needs it, to make room for them).
        Pet() {}

        // And this one takes a name and a kind.
        Pet(string n, string k) {
                name = n;
                kind = k;
        }

        // Methods: functions that belong to the class, and use its variables.
        void feed() {
                hunger = max(0, hunger - 3);
                println(name + " eats. Yum!");
        }

        void play() {
                hunger += 2;
                println(name + " plays and gets hungrier.");
        }

        string describe() {
                string mood = hunger == 0 ? "full" : hunger < 5 ? "happy" : "hungry";
                return name + " the " + kind + " is " + mood + " (hunger " + hunger + ").";
        }
}

Pet rex("Rex", "dog");
Pet tom("Tom", "cat");
println(rex.describe());
rex.feed();
rex.feed();
println(rex.describe());
tom.play();
println(tom.describe());

// A list of objects: every pet gets fed.
Pet[] pets = {rex, tom, Pet("Goldie", "fish")};
println();
foreach (Pet p : pets) println(p.describe());

// Try this:
//   1. Give Pet a sleep() method that makes hunger go up by 1, and call it.
//   2. Add a happiness variable that play() raises.
//   3. Make a class Player with a name, a score and an addPoints(int) method.
