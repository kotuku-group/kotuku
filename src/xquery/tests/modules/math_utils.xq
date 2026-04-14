module namespace math = "http://example.com/math";

declare function math:square($x as xs:integer) as xs:integer {
   $x * $x
};

declare function math:cube($x) {
   $x * $x * $x
};

declare variable $math:pi := 3.14159;
