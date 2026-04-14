module namespace str = "http://example.com/strings";

declare function str:reverse($s as xs:string) as xs:string {
   fn:reverse($s)
};

declare function str:uppercase($s as xs:string) as xs:string {
   fn:upper-case($s)
};
