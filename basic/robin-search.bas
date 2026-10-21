10 b$="@WORD@":t=22:s=64
20 forb=@FIRST@to@LAST@
100 a=rnd(-b):a$=""
110 a%=rnd(a)*t:ifa%thena$=a$+chr$(a%+s):iflen(a$)<=len(b$)then110
120 ifa$=b$thenprintb,b$
130 next
