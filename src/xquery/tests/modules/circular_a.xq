module namespace a = "http://example.com/circular-a";
import module namespace b = "http://example.com/circular-b" at "circular_b.xq";

declare function a:call-b() { b:call-a() };
