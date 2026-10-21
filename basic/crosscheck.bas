100 data 125708,33435700,17059266
110 for i=1to3:read a:gosub 200:next
120 b=rnd(-33721604):b=rnd(1):t=22:a%=b*t:print int(b*22);a%
130 for n=1 to 5:a=rnd(-n):print a;:for j=1to3:print rnd(1);:next:print:next
140 b$="e":t=22:s=64
150 for b=1to3000
160 a=rnd(-b):a$=""
170 a%=rnd(a)*t:ifa%thena$=a$+chr$(a%+s):iflen(a$)<=len(b$)then170
180 ifa$=b$thenprintb,b$
190 next:goto 990
200 a=rnd(-a)
210 a=int(rnd(a)*22):if a then print chr$(a+64);:goto210
220 print:return
990 end
