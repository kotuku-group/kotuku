module namespace b = "http://example.com/circular-b";
import module namespace a = "http://example.com/circular-a" at "circular_a.xq";

declare function b:call-a() { a:call-b() };
