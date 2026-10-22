// A function using a global the script hasn't reached yet: numbers are 0, objects have no
// value (a null pointer, with a note saying why).
// exit: 1
println(score());
println(title());
int points = 5;
string name = "Ada";
int score() { return points; }
string title() { return "Dr " + name; }
