import sys,time,socket; sys.path.insert(0,'.')
from rsp import RSP
def connect(port):
    for _ in range(60):
        try: return RSP(port=port,timeout=30)
        except OSError: time.sleep(0.5)
    raise SystemExit("no emu")
port=int(sys.argv[1]) if len(sys.argv)>1 else 3401
r=connect(port)
CC=0x400371f8
ssp=r.rdword(CC+0x0c)
print("CC saved sp=0x%08x  state=0x%x"%(ssp,r.rdword(CC+0x28)),flush=True)
def iscode(w):
    return (0x00003000<=w<0x00160000) or (0x40000000<=w<0x40050000)
lo=ssp-0x40; hi=ssp+0x600
print("scan stack 0x%08x..0x%08x for code pointers:"%(lo,hi),flush=True)
a=lo
while a<hi:
    w=r.rdword(a)
    if iscode(w):
        print("  [0x%08x] = 0x%08x  <CODE>"%(a,w),flush=True)
    a+=4
r.cmd('D')
