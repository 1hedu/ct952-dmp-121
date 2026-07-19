import sys,time,socket; sys.path.insert(0,'.')
from rsp import RSP
def connect(port):
    for _ in range(60):
        try: return RSP(port=port,timeout=40)
        except OSError: time.sleep(0.5)
    raise SystemExit("no emu")
port=int(sys.argv[1]) if len(sys.argv)>1 else 3401
r=connect(port)
def contb(to):
    r.s.settimeout(to)
    try: return r.cont()
    except socket.timeout:
        r.s.sendall(b'\x03')
        try: return r.read()
        except: return None
CC=0x400371f8
print("CC state=0x%x (1=SLEEP)  saved_sp=0x%08x"%(r.rdword(CC+0x28), r.rdword(CC+0x0c)),flush=True)
PUT=0x4001de5c; GET=0x5969c
def thr(sp):
    if 0x400359d8<=sp<0x400371d8: return "CC"
    return "0x%08x"%sp
# --- who POSTS (put primitive) ---
print("\n== PUT primitive 0x%x hits (o0=obj,o1=val, sp=thread, i7=caller) =="%PUT,flush=True)
r.bp(PUT)
for i in range(12):
    rep=contb(6)
    if rep is None: print("  [%d] no PUT in 6s"%i,flush=True); break
    g=r.regs()
    print("  [%d] o0=0x%08x o1=0x%08x sp=0x%08x(%s) i7=0x%08x"%(
        i,g[8],g[9],g[14],thr(g[14]),g[31]),flush=True)
r.rmbp(PUT)
# --- does CC ever wake (hit GET)? ---
print("\n== GET wrapper 0x%x hits (sp=thread, i7=caller) =="%GET,flush=True)
r.bp(GET)
ccwoke=False
for i in range(12):
    rep=contb(6)
    if rep is None: print("  [%d] no GET in 6s"%i,flush=True); break
    g=r.regs(); t=thr(g[14])
    if t=="CC": ccwoke=True
    print("  [%d] sp=0x%08x(%s) i7=0x%08x"%(i,g[14],t,g[31]),flush=True)
r.rmbp(GET)
print("\nCC woke (hit GET) during probe:",ccwoke,flush=True)
print("CC state now=0x%x"%r.rdword(CC+0x28),flush=True)
r.cmd('D')
