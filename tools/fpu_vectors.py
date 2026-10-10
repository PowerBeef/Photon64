#!/usr/bin/env python3
"""Independent exact-rational COP1 arithmetic vectors (no host floats/SoftFloat)."""
from fractions import Fraction as F
from math import isqrt
import random
from pathlib import Path
rng = random.Random(4300)
rows = []
def power(n): return F(1 << n) if n >= 0 else F(1, 1 << -n)
def decode(u,n):
 p,e,b=(23,8,127) if n==32 else (52,11,1023)
 exponent=(u>>p)&((1<<e)-1);m=u&((1<<p)-1)
 return (-1 if u>>(n-1) else 1)*F(m+(1<<p) if exponent else m)*power((exponent or 1)-b-p)
def log2(x):
 e=x.numerator.bit_length()-x.denominator.bit_length()
 return e-1 if x<power(e) else e
def round_bits(x,n,rm,root=False,zero_sign=0):
 p,eb,b=(23,8,127) if n==32 else (52,11,1023); emin=1-b;emax=b
 sign=x<0;x=abs(x);flag=0
 if not x:return zero_sign<<(n-1),0
 exponent=log2(x)//2 if root else log2(x)
 shift=max(exponent,emin)-p
 y=x/power(2*shift if root else shift)
 q=isqrt(y.numerator//y.denominator) if root else y.numerator//y.denominator
 exact=F(q*q)==y if root else F(q)==y
 if not exact:
  halfcmp=4*y-F((2*q+1)**2) if root else y-F(2*q+1,2)
  if (rm==0 and (halfcmp>0 or (halfcmp==0 and q&1))) or (rm==2 and not sign) or (rm==3 and sign):q+=1
  flag=1
 if q==1<<(p+1): q>>=1;exponent+=1
 if exponent>emax:
  inf=rm==0 or (rm==2 and not sign) or (rm==3 and sign)
  return (int(sign)<<(n-1))|((((1<<eb)-1)<<p) if inf else ((((1<<eb)-2)<<p)|((1<<p)-1))),5
 # VR4300 flushes even exact denormal results; tiny rounded-to-zero keeps sign.
 if exponent<emin and q<(1<<p):
  minimal=(rm==2 and not sign) or (rm==3 and sign)
  return (int(sign)<<(n-1))|((1<<p) if minimal else 0),3
 ef=max(exponent,emin)+b if q>=(1<<p) else 0
 return (int(sign)<<(n-1))|(ef<<p)|(q&((1<<p)-1)),flag

def add(fmt,fn,a,b,rm):
 n=32 if fmt==16 else 64;x,y=decode(a,n),decode(b,n)
 if fn==0:z=x+y
 elif fn==1:z=x-y
 elif fn==2:z=x*y
 elif fn==3:z=x/y
 else:z=x
 zs=0
 if not z:
  if fn in (0,1):
   # Exact cancellation is negative zero only under RM (same-signed zeros special).
   bs=b ^ ((1<<(n-1)) if fn==1 else 0)
   zs= (a>>(n-1)) if not x and not y and (a>>(n-1))==(bs>>(n-1)) else int(rm==3)
  elif fn in (2,3):zs=(a^b)>>(n-1)
  else:zs=a>>(n-1)
 bits,flags=round_bits(z,n,rm,fn==4,zs)
 control=(1<<24)|rm
 fcr=control|flags<<12|flags<<2
 rows.append((fmt,fn,a,b,control,bits,fcr,0))
for fmt,n in ((16,32),(17,64)):
 p,eb=(23,8) if n==32 else (52,11)
 def rand():return rng.getrandbits(1)<<(n-1)|rng.randrange(1,(1<<eb)-1)<<p|rng.getrandbits(p)
 for rm in range(4):
  for fn in range(5):
   for _ in range(80):
    a,b=rand(),rand()
    if fn==4:a&=(1<<(n-1))-1
    add(fmt,fn,a,b,rm)
  # Zero cancellation and ties, directions and overflow/underflow boundaries.
  for a,b in ((0,0),(1<<(n-1),1<<(n-1)),(1<<p,1<<p),((1<<(n-1))|1<<p,1<<p)):
   for fn in (0,1,2):add(fmt,fn,a,b,rm)
# Policy cases: all five enabled exceptions preserve destination; NaNs, denormals, unsupported formats.
sentinel=0x123456789ABCDEF0
for fmt,n in ((16,32),(17,64)):
 p,eb,bias=(23,8,127) if n==32 else (52,11,1023)
 one=bias<<p;inf=((1<<eb)-1)<<p;qnan=inf|1;snan=inf|1<<(p-1);maxfinite=inf-1
 cases=[(0,one,1,0,1<<17,1),(0,one,qnan,0,1<<17,1),(0,one,snan,0,16,0),(0,one,snan,1<<11,16,1),
  (3,one,0,1<<10,8,1),(3,0,0,1<<11,16,1),(2,maxfinite,one+1<<0,1<<9,5,1),
  (3,one,one+(1<<p)//2,1<<7,1,1),(2,1<<p,1<<p,0,1<<17,1),(2,1<<p,1<<p,(1<<24)|(1<<8),1<<17,1)]
 for fn,a,b,control,flags,trap in cases:
  if flags==1<<17: fcr=control|flags;bits=sentinel
  else:
   fcr=control|flags<<12|((flags & ~(control>>7))<<2)
   bits=sentinel if trap else (0x7FBFFFFF if n==32 else 0x7FF7FFFFFFFFFFFF)
  rows.append((fmt,fn,a,b,control,bits,fcr,trap))
# Conversion boundary and directed rounding cases, expected by integer mathematics.
for rm in range(4):
 for fn,wide in ((36,False),(37,True)):
  for value in (F(1,2),F(3,2),F(-3,2),F(-2147483648)-F(1,4),F(2147483648)-F(1,4),F(-9007199254740992),F(9007199254740992)):
   a,_=round_bits(value,64,0);v=value.numerator//value.denominator
   if rm==0:
    rem=value-v
    if rem>F(1,2) or (rem==F(1,2) and v&1):v+=1
   elif rm==1 and value<0 and value!=v:v+=1
   elif rm==2 and value!=v:v+=1
   lo,hi=(-2**53,2**53) if wide else (-2**31,2**31)
   trap=not lo<=v<hi;flags=(1<<17) if trap else (0x1004 if F(v)!=value else 0)
   bits=sentinel if trap else v&((1<<64)-1 if wide else (1<<32)-1)
   rows.append((17,fn,a,0,rm,bits,rm|flags,int(trap)))
# Integer to floating point: positive/negative ties and the VR4300 L range.
for fmt in (20,21):
 for rm in range(4):
  for fn,n in ((32,32),(33,64)):
   values=[0,1,-1,2**24+1,-(2**24+1),2**31-1,-2**31]
   if fmt==21:values += [2**53+1,-(2**53+1),2**55-1,-2**55,2**55,-2**55-1]
   for v in values:
    trap=fmt==21 and not -2**55<=v<2**55
    r,flags=round_bits(F(v),n,rm)
    rows.append((fmt,fn,v&((1<<64)-1),0,rm,sentinel if trap else r,rm|((1<<17) if trap else flags<<12|flags<<2),int(trap)))
# Quiet/signaling compare predicates allow denormals and use legacy NaN polarity.
for fmt,n in ((16,32),(17,64)):
 p,eb,bias=(23,8,127) if n==32 else (52,11,1023)
 inf=((1<<eb)-1)<<p
 for fn in range(48,64):
  for a,b in ((inf|1,bias<<p),(inf|(1<<(p-1)),bias<<p),(1,0)):
   nan=(a>>p)&((1<<eb)-1)==(1<<eb)-1
   invalid=nan and (bool(fn&8) or bool(a&(1<<(p-1))))
   cond= bool((fn&1) and nan) or bool((fn&2) and not nan and a==b) or bool((fn&4) and not nan and decode(a,n)<decode(b,n))
   fcr=(16<<12|16<<2 if invalid else 0)|(int(cond)<<23)
   rows.append((fmt,fn,a,b,0,sentinel,fcr,0))
lines=['// Generated by tools/fpu_vectors.py using exact Fraction/isqrt arithmetic.','static const struct { u32 fmt, fn; u64 a, b; u32 control; u64 result; u32 fcr, trap; } fpu_vectors[] = {']
for fmt,fn,a,b,c,r,f,t in rows:lines.append(f'  {{{fmt}, {fn}, 0x{a:X}ull, 0x{b:X}ull, 0x{c:X}, 0x{r:X}ull, 0x{f:X}, {t}}},')
lines.append('};');Path('tools/fpu_vectors.h').write_text('\n'.join(lines)+'\n');print(len(rows),'vectors')
