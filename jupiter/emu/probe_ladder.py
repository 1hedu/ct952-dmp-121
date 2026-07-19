import sys,time,socket; sys.path.insert(0,'.')
from rsp import RSP
def connect(port):
    for _ in range(60):
        try: return RSP(port=port,timeout=40)
        except OSError: time.sleep(0.5)
    raise SystemExit("no emu")
port=int(sys.argv[1]) if len(sys.argv)>1 else 3403
r=connect(port)
def contb(to):
    r.s.settimeout(to)
    try: return r.cont()
    except socket.timeout:
        r.s.sendall(b'\x03')
        try: return r.read()
        except: return None
# ladder of addresses along INITIAL_PowerONStatus init sequence
ladder=[0x41ad4,0x41ae4,0x41b04,0x41b34,0x41b58,0x41c00,0x41d00,0x41e00]
# which does the CC thread hit? set all, continue, tally
for a in ladder: r.bp(a)
from collections import Counter
hits=Counter()
for i in range(30):
    rep=contb(4)
    if rep is None: continue
    pc=r.regs()[68]
    hits[pc]+=1
for a in ladder: r.rmbp(a)
print("hit tally over 30 continues:",flush=True)
for a in ladder:
    print("  0x%08x : %d"%(a,hits.get(a,0)),flush=True)
# also report current CC saved sp + top return chain
CC=0x400371f8; ssp=r.rdword(CC+0x0c)
print("CC saved_sp=0x%08x state=0x%x"%(ssp,r.rdword(CC+0x28)),flush=True)
r.cmd('D')
