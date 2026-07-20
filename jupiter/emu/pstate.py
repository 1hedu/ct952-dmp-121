import struct,sys
s=open(sys.argv[1]).read() if False else open(sys.argv[1],'rb').read()
base=[i for i in range(0,len(s)-3) if struct.unpack('>I',s[i:i+4])[0]==0x591b4][0]-0x20e18
def off(a): return base+(a-0x40000000)
def b(a): return s[off(a)]
def w(a): return struct.unpack('>I',s[off(a):off(a)+4])[0]
au=w(0x40020ec8)
mode='-'
if 0x40024c20<=au<0x40024e00: mode='0x%02x'%w(au)
print("t=%-8s activeUI=%08x mode=%s  chooseMedia=%d ponInit=%d state8gate=%d ccflag=%08x loadingActive=%d qcount=%d"%(
  sys.argv[2] if len(sys.argv)>2 else '?', au, mode, b(0x40031b9c), b(0x40023a10), b(0x40022F97), w(0x40026EA4), b(0x40039344), w(0x400329fc)))
