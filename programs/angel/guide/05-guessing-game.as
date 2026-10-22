#!/rom/bin/angel
// Lesson 5: A guessing game
//
// Your first game, made of what the first four lessons showed. The computer picks a secret
// number, and you guess it, told "higher" or "lower" each time.

// random(1, 100) picks a whole number from 1 to 100, a different one each run.
int secret = random(1, 100);
int tries = 0;

println("I'm thinking of a number from 1 to 100. Can you guess it?");
println("(Press Enter without a number to give up.)");

// while (true) would go on forever: break is the way out.
while (true) {
        string answer = input("Your guess? ");
        if (answer == "") {
                println("The number was " + secret + ". Better luck next time!");
                break;
        }
        int guess = int(parseInt(answer));
        tries++;
        if (guess < secret) {
                println("Higher!");
        } else if (guess > secret) {
                println("Lower!");
        } else {
                println("Yes! It was " + secret + ". You got it in " + tries + " tries.");
                if (tries <= 7) println("That's very good: 7 tries is always enough, if you halve each time.");
                break;
        }
}

// Try this:
//   1. Make the number go up to 1000. How many tries do you need now?
//   2. Give only 10 tries: after the tenth wrong guess, the game says the number and ends.
//   3. Say "very close!" when the guess is within 3 of the secret.
