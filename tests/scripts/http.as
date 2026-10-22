// HTTP against run.py's server on loopback ($TEST_HTTP).
string base = env("TEST_HTTP");
print(Http::get(base + "/hello"));
println(Http::get(base + "/chunked"));
println(Http::get(base + "/close"));
print(Http::get(base + "/moved"));
println(Http::post(base + "/echo", "name=" + Http::encode("Ada Lovelace")));
println(Http::post(base + "/echo", "{}", "application/json"));
println(Http::post(base + "/form", "a=1"));

HttpResponse@ r = Http::request("get", base + "/hello");
println(r.status + " " + r.reason + " " + r.header("content-type") + " " + r.header("X-TEST") + " [" +
        r.header("None") + "]");
println(r.url == base + "/hello");
println(r.headers().contains("Content-Length: 14"));
@r = Http::request("PUT", base + "/echo/put", "data", {"X-Name: angel", "Content-Type: text/plain"});
println(r.body);
@r = Http::request("HEAD", base + "/hello");
println(r.status + " [" + r.body + "]");
@r = Http::request("GET", base + "/missing");
println(r.status + " " + r.body);
@r = Http::request("GET", base + "/moved");
println(r.url == base + "/hello");

try {
        Http::get(base + "/missing");
} catch {
        println(getExceptionInfo().replace(base, "BASE"));
}
try {
        Http::get(base + "/loop");
} catch {
        println(getExceptionInfo().replace(base, "BASE"));
}
string[] bad = {"https://example.com/", "ftp://example.com/", "http:///path", "http://host:99999/"};
for (uint i = 0; i < bad.length(); i++) {
        try {
                Http::get(bad[i]);
        } catch {
                println(getExceptionInfo());
        }
}
