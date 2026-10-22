#!/rom/bin/angel
// Guess the number: the computer thinks of a number from 1 to 100, and you guess it.
// Run it with:  angel guess.as

int secret = random(1, 100);
int tries = 0;

println("I'm thinking of a number from 1 to 100.");
while (true) {
        string answer = input("Your guess? ");
        if (answer == "") {
                println("Bye! It was " + secret + ".");
                break;
        }
        int guess = int(parseInt(answer));
        tries++;
        if (guess < secret) {
                println("Higher!");
        } else if (guess > secret) {
                println("Lower!");
        } else {
                println("Yes, " + secret + "! You got it in " + tries + (tries == 1 ? " try." : " tries."));
                break;
        }
}
