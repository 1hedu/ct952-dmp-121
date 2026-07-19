import sys,time,socket; sys.path.insert(0,'.')
from rsp import RSP
def connect(port):
    for _ in range(90):
        try: return RSP(port=port,timeout=40)
        except OSError: time.sleep(0.5)
    raise SystemExit("no emu")
port=int(sys.argv[1]) if len(sys.argv)>1 else 3402
r=connect(port)
def wr(addr,data):  # data = bytes
    hexs=''.join('%02x'%b for b in data)
    return r.cmd('M%x,%x:%s'%(addr,len(data),hexs))
DBGEN=0x40021eb0; DBGFLAG=0x40021dd4
print("before: _bDBGEnable=0x%02x __dwDebugFlag=0x%08x"%(r.rdmem(DBGEN,1)[0], r.rdword(DBGFLAG)),flush=True)
print("wr enable:",wr(DBGEN,b'\x01'),flush=True)
print("wr flag  :",wr(DBGFLAG,b'\xff\xff\xff\xff'),flush=True)
print("after : _bDBGEnable=0x%02x __dwDebugFlag=0x%08x"%(r.rdmem(DBGEN,1)[0], r.rdword(DBGFLAG)),flush=True)
def contb(to):
    r.s.settimeout(to)
    try: return r.cont()
    except socket.timeout:
        r.s.sendall(b'\x03')
        try: return r.read()
        except: return None
print("continuing to let firmware narrate...",flush=True)
for i in range(6):
    contb(5)
    print("  [chunk %d] pc=0x%08x"%(i,r.regs()[68]),flush=True)
r.cmd('D')
print("done",flush=True)
