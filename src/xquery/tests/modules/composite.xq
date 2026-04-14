module namespace comp = "http://example.com/composite";

import module namespace math = "http://example.com/math" at "math_utils.xq";

declare function comp:area($r) {
   $math:pi * math:square($r)
};
