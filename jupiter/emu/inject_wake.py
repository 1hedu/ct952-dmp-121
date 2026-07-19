import sys,time,socket; sys.path.insert(0,'/tmp')
from rsp import RSP
def connect():
    for _ in range(30):
        try: return RSP(timeout=25)
        except OSError: time.sleep(0.5)
    raise SystemExit("no emu")
r=connect()
def setr(n,v): r.cmd('P%x=%08x'%(n,v))
def contb(to):
    r.s.settimeout(to)
    try: return r.cont()
    except socket.timeout: return None
g0=r.regs()
print("before: pc=0x%08x  CCthread state=0x%x  mbox+0x3c=0x%x"%(
      g0[68], r.rdword(0x400371f8+40), r.rdword(0x4003386c)),flush=True)
# breakpoints: OSDSS reached (WIN), CC re-blocks (woke), poster returned (no switch)
for a in (0x591b4,0x5969c,0x407ffff0): r.bp(a)
# inject call to eCos mbox-put 0x4001de5c(msg0=1,msg1=1); keep idle's real stack
setr(15,0x407ffff0)      # o7 = sentinel return
setr(8,0x00000001)       # o0 = msg word0
setr(9,0x00000001)       # o1 = msg word1
setr(68,0x4001de5c); setr(69,0x4001de60)  # pc,npc = put primitive
print("injected call -> mbox-put 0x4001de5c(1,1); continue...",flush=True)
rep=contb(20)
if rep is None:
    r.s.sendall(b'\x03'); r.s.settimeout(10)
    try: rep=r.read()
    except: rep='?'
    g=r.regs(); print("TIMEOUT; interrupted at pc=0x%08x"%g[68],flush=True)
else:
    g=r.regs(); pc=g[68]
    tag={0x591b4:'*** OSDSS_Monitor REACHED ***',0x5969c:'CC woke + re-blocked (mbox-get)',
         0x407ffff0:'put returned, no context switch'}.get(pc,'pc=0x%08x'%pc)
    print("stop: %s (pc=0x%08x)"%(tag,pc),flush=True)
    print("  CCthread state now=0x%x  mbox+0x3c=0x%x"%(
          r.rdword(0x400371f8+40), r.rdword(0x4003386c)),flush=True)
    # if CC re-blocked or reached OSDSS, keep going a few rounds to see the loop
    for i in range(6):
        for a in (0x591b4,0x5969c,0x407ffff0):
            pass
        rep=contb(15)
        if rep is None: print("  round%d: no bp in 15s"%i,flush=True); break
        pc=r.regs()[68]
        t={0x591b4:'OSDSS!!!',0x5969c:'mbox-get(reblock)',0x407ffff0:'sentinel'}.get(pc,'0x%08x'%pc)
        print("  round%d: %s"%(i,t),flush=True)
        if pc==0x591b4: print("  >>> SCREENSAVER MONITOR RUNS <<<",flush=True); break
r.cmd('D')
