// What a drawing call refuses, as exceptions.
try { Window big("Too big", 2000, 100); } catch { println(getExceptionInfo()); }
try { Image none(0, 5); } catch { println(getExceptionInfo()); }
Window w;
println(w.width() + "x" + w.height());
try { w.fillPolygon({1, 2, 3}, Color::Red); } catch { println(getExceptionInfo()); }
Image@ missing;
try { w.image(missing, 0, 0); } catch { println(getExceptionInfo()); }
try { Image picture("picture.png"); } catch { println(getExceptionInfo()); }
try { w.save("x.png"); } catch { println(getExceptionInfo()); }
class Counter : Game {
        int ticks = 0;
        void tick() { ticks++; if (ticks == 3) w.stop(); }
}
Counter c;
w.run(c, 200);
println("ran " + c.ticks + " ticks");
