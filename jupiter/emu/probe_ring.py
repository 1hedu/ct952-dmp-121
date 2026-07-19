import sys,time; sys.path.insert(0,'.')
from rsp import RSP
def connect(port):
    for _ in range(60):
        try: return RSP(port=port,timeout=40)
        except OSError: time.sleep(0.5)
    raise SystemExit("no emu")
port=int(sys.argv[1]) if len(sys.argv)>1 else 3403
r=connect(port)
hdr=r.rdword(0x40032508)
widx=r.rdword(0x400324d8)>>16 if False else (r.rdmem(0x400324d8,2)[0]<<8 | r.rdmem(0x400324d8,2)[1])
ridx=(r.rdmem(0x400324da,2)[0]<<8 | r.rdmem(0x400324da,2)[1])
print("_pDBG_Header1=0x%08x  _wInfoWIdx=%d  (raw 0x400324d8..)"%(hdr,widx),flush=True)
STRIDE=96; N=64
print("dumping %d ring descriptors (bSize@+0, bData@+1):"%N,flush=True)
for i in range(N):
    base=hdr+i*STRIDE
    sz=r.rdmem(base,1)[0]
    if sz==0 or sz>90: continue
    data=r.rdmem(base+1,sz)
    txt=''.join(chr(c) if 32<=c<127 else ('\\n' if c==10 else '.') for c in data)
    print("  [%2d] sz=%2d: %s"%(i,sz,txt),flush=True)
r.cmd('D')
