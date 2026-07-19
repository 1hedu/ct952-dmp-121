import sys,time,socket,os; sys.path.insert(0,'.')
from rsp import RSP
def connect(port):
    for _ in range(90):
        try: return RSP(port=port,timeout=60)
        except OSError: time.sleep(0.5)
    raise SystemExit("no emu")
port=int(sys.argv[1]) if len(sys.argv)>1 else 3403
uartf=sys.argv[2] if len(sys.argv)>2 else '/tmp/fw3.txt'
r=connect(port)
def wr(addr,data):
    return r.cmd('M%x,%x:%s'%(addr,len(data),''.join('%02x'%b for b in data)))
def contb(to):
    r.s.settimeout(to)
    try: return r.cont()
    except socket.timeout:
        r.s.sendall(b'\x03')
        try: return r.read()
        except: return None
DBGEN=0x40021eb0; DBGFLAG=0x40021dd4
wr(DBGEN,b'\x01'); wr(DBGFLAG,b'\xff\xff\xff\xff')
print("debug enabled: _bDBGEnable=0x%02x flag=0x%08x"%(r.rdmem(DBGEN,1)[0],r.rdword(DBGFLAG)),flush=True)
def usz():
    try: return os.path.getsize(uartf)
    except: return 0
print("tick@start=0x%x  uart=%dB"%(r.rdword(0x4002e32c),usz()),flush=True)
for i in range(20):
    contb(8)
    tk=r.rdword(0x4002e32c); u=usz()
    print("  chunk %2d: tick=0x%08x  CCstate=0x%x  uart=%dB"%(i,tk,r.rdword(0x400371f8+0x28),u),flush=True)
    if u>0:
        print("  *** UART OUTPUT APPEARED (main loop reached / firmware narrating) ***",flush=True)
        break
r.cmd('D')
print("detached",flush=True)
