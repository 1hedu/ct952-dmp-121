import sys,time,socket; sys.path.insert(0,'.')
from rsp import RSP
def connect(port):
    for _ in range(90):
        try: return RSP(port=port,timeout=60)
        except OSError: time.sleep(0.5)
    raise SystemExit("no emu")
port=int(sys.argv[1]) if len(sys.argv)>1 else 3405
r=connect(port)
def wr(addr,data): return r.cmd('M%x,%x:%s'%(addr,len(data),''.join('%02x'%b for b in data)))
def contb(to):
    r.s.settimeout(to)
    try: return r.cont()
    except socket.timeout:
        r.s.sendall(b'\x03')
        try: return r.read()
        except: return None
# allocate ring exactly like _DBG_ResetDRAMDebugMessage (0x13ea0)
RING=0x4004c000
wr(0x40032508, RING.to_bytes(4,'big'))   # _pDBG_Header1
wr(0x400324d8, b'\x00\x00')              # _wInfoWIdx=0
wr(0x400324ce, b'\x00\x00')              # (other index)
wr(0x40021eb0, b'\x01')                  # _bDBGEnable=1
wr(0x40021dd4, b'\xff\xff\xff\xff')      # __dwDebugFlag=all
wr(RING+5, b'\x00')                      # ring[0].? clear
print("ring set: _pDBG_Header1=0x%08x _bDBGEnable=0x%x flag=0x%08x"%(
    r.rdword(0x40032508), r.rdmem(0x40021eb0,1)[0], r.rdword(0x40021dd4)),flush=True)
STRIDE=96
def dump_ring(tag):
    widx=(r.rdmem(0x400324d8,2)[0]<<8)|r.rdmem(0x400324d8,2)[1]
    print("--- ring @%s  _wInfoWIdx=%d ---"%(tag,widx),flush=True)
    got=0
    for i in range(min(widx if widx else 40, 64)):
        base=RING+i*STRIDE
        sz=r.rdmem(base,1)[0]
        if sz==0 or sz>90: continue
        data=r.rdmem(base+1,sz)
        txt=''.join(chr(c) if 32<=c<127 else ('\\n' if c in(10,13) else '.') for c in data)
        print("  [%2d] %s"%(i,txt),flush=True); got+=1
    if not got: print("  (no messages buffered)",flush=True)
# let it run and accumulate messages
for k in range(4):
    contb(6)
    dump_ring("tick=0x%x"%r.rdword(0x4002e32c))
r.cmd('D')
