#!/rom/bin/angel
// #! is skipped; #include adds a file's declarations (each file once, relative paths).
#include "lib/twice.as"
#include "lib/half.as"
  #include "lib/twice.as"
string s = "#include \"not a directive\"";
/*
#include "inside a comment"
*/
println(twice(21));
println(half(21));
println(s);
