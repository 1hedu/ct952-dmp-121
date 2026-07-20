import sys,time,socket; sys.path.insert(0,'.')
from rsp import RSP
def connect(port):
    for _ in range(90):
        try: return RSP(port=port,timeout=60)
        except OSError: time.sleep(0.5)
    raise SystemExit("no emu")
port=int(sys.argv[1]) if len(sys.argv)>1 else 3410
r=connect(port)
def contb(to):
    r.s.settimeout(to)
    try: return r.cont()
    except socket.timeout:
        r.s.sendall(b'\x03')
        try: return r.read()
        except: return None
# EHCI init reads CAPLENGTH at the call site 0xb0260 (o0=tag). Catch it.
r.bp(0xb0260)
print("waiting for ehci_init CAPLENGTH read (bp 0xb0260)...",flush=True)
rep=contb(40)
if rep is None:
    print("  NOT reached in 40s (pc=0x%08x)"%r.regs()[68],flush=True); r.cmd('D'); sys.exit()
g=r.regs(); tag=g[8]
print("  HIT 0xb0260  tag(o0)=0x%08x  [tag+8]=0x%08x"%(tag, r.rdword(tag+8)),flush=True)
r.rmbp(0xb0260)
# step into helper, stop at the actual load 0xaf174 to read the MMIO address
r.bp(0xaf174)
rep=contb(10)
if rep is not None and r.regs()[68]==0xaf174:
    g=r.regs(); g2=g[2]; g3=g[3]
    print("  helper 0xaf174: g2=0x%08x g3=0x%08x -> reads addr 0x%08x"%(g2,g3,g2+g3),flush=True)
    print("  => EHCI register handle/base region near 0x%08x"%g2,flush=True)
r.rmbp(0xaf174)
r.cmd('D')
