10 b$="@WORD@"
20 forb=@FIRST@to@LAST@
100 a=rnd(-b):a$=""
110 a=int(rnd(a)*22):ifathena$=a$+chr$(a+64):iflen(a$)<=len(b$)then110
120 ifa$=b$thenprintb,b$
130 next
