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
RING=0x4004c000; STRIDE=96; NSLOT=42
def slots():
    raw=r.rdmem(RING, STRIDE*NSLOT)          # ONE bulk read
    out={}
    for i in range(NSLOT):
        seg=raw[i*STRIDE:(i+1)*STRIDE]
        m=re.search(rb'[\x20-\x7e]{4,}', seg)
        if m: out[i]=m.group().decode('ascii','replace')
    return out
seen=set(); prev={}
niter=int(sys.argv[2]) if len(sys.argv)>2 else 60
for k in range(niter):
    cur=slots()
    for i,msg in cur.items():
        if prev.get(i)!=msg or msg not in seen:
            if msg not in seen:
                seen.add(msg); print("%s"%msg,flush=True)
    prev=cur
    contb(2)
print("=== %d distinct lines ==="%len(seen),flush=True)
r.cmd('D')
