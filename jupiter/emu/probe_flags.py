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
POM=0x40023a10   # __bPOWERONMENUInitial (doc 10.47)
OSDT=0x400239b8  # __dwOSDSSCheckTime
SSMODE=0x400239c4# _bOSDSSScreenSaverMode
CLK=0x400398f8   # eCos clock counter
def snap(tag):
    print("%-8s clk=0x%08x  POMInit=0x%02x  OSDSSChkTime=0x%08x  SSmode=0x%02x"%(
        tag, r.rdword(CLK), r.rdword(POM)&0xff, r.rdword(OSDT), r.rdword(SSMODE)&0xff),flush=True)
snap("start")
# continue in chunks, watch flags evolve
for i in range(8):
    contb(5)
    snap("t+%ds"%((i+1)*5))
# is OSDSS_Monitor (0x591b4) ever reached?
print("\nbreakpoint OSDSS_Monitor 0x591b4 ...",flush=True)
r.bp(0x591b4)
rep=contb(20)
if rep is None:
    print("  OSDSS_Monitor NOT reached in 20s (sp=0x%08x pc=0x%08x)"%(r.regs()[14],r.regs()[68]),flush=True)
else:
    g=r.regs(); print("  *** OSDSS_Monitor REACHED sp=0x%08x i7=0x%08x ***"%(g[14],g[31]),flush=True)
r.rmbp(0x591b4)
snap("final")
r.cmd('D')
