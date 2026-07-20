import sys,time,socket,re; sys.path.insert(0,'.')
from rsp import RSP
r=RSP(port=int(sys.argv[1]) if len(sys.argv)>1 else 3406, timeout=60)
def contb(to):
    r.s.settimeout(to)
    try: return r.cont()
    except socket.timeout:
        r.s.sendall(b'\x03')
        try: return r.read()
        except: return None
RING=0x4004c000; STRIDE=96; NSLOT=32
def read_slots():
    out={}
    for i in range(NSLOT):
        base=RING+i*STRIDE
        try: raw=r.rdmem(base,STRIDE)
        except: break
        m=re.search(rb'[\x20-\x7e]{4,}', raw)
        if m: out[i]=m.group().decode('ascii','replace')
    return out
seen=set(); log=[]
prev={}
for k in range(40):
    slots=read_slots()
    for i,msg in slots.items():
        key=(i,msg)
        if prev.get(i)!=msg and msg not in seen:
            seen.add(msg); log.append(msg); print("  "+msg,flush=True)
    prev=slots
    contb(3)
print("=== %d distinct messages ==="%len(log),flush=True)
r.cmd('D')
