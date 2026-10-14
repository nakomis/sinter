\ Every control-flow shape and a literal of each width, in one image.
: sign ( n -- -1|0|1 ) dup 0< if drop -1 else 0= if 0 else 1 then then ;
: count-down ( n -- ) begin 1- dup 0= until drop ;
: gcd ( a b -- g ) begin dup while swap over mod repeat drop ;
: sum ( n -- total ) 0 swap 0 do i + loop ;
: widths 1 -1 4294967296 drop drop drop ;
-5 sign drop  10 count-down  1071 462 gcd drop  101 sum result!  widths
