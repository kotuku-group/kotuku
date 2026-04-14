module namespace cycle = "http://example.com/cycle";

declare variable $cycle:value :=
   if ($cycle:value) then 1 else 0;
