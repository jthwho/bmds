// Other programs: run() with the output on the screen, output() given back.
int status = run("echo from run");
println(status);
println(run("exit 3"));
println(output("echo captured").trim());
