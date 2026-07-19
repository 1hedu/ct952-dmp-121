import sys,time,socket; sys.path.insert(0,'.')
from rsp import RSP
def connect(port):
    for _ in range(60):
        try: return RSP(port=port,timeout=30)
        except OSError: time.sleep(0.5)
    raise SystemExit("no emu")
port=int(sys.argv[1]) if len(sys.argv)>1 else 3401
r=connect(port)
CC=0x400371f8
# eCos Cyg_Thread: scan first 0x30 bytes for a stack-looking pointer (0x40036xxx)
print("CC thread object 0x%08x dump:"%CC,flush=True)
words=[r.rdword(CC+4*i) for i in range(16)]
for i,w in enumerate(words):
    tag=""
    if 0x40034000<=w<0x40039000: tag=" <-stack?"
    if i==10: tag+=" [+0x28 state]"
    print("  +0x%02x = 0x%08x%s"%(i*4,w,tag),flush=True)
# state at +0x28 (word index 10)
print("state(+0x28)=0x%x (0=RUN,1=SLEEP,2=CNTSLEEP,4=SUSP)"%words[10],flush=True)
# find saved sp: likely +0x00
ssp=words[0]
print("assuming saved sp = +0x00 = 0x%08x"%ssp,flush=True)
def unwind(sp,depth=16):
    print("  unwind from sp=0x%08x:"%sp,flush=True)
    for d in range(depth):
        if not (0x40030000<=sp<0x40040000):
            print("    [stop: sp=0x%08x out of range]"%sp,flush=True); break
        fp=r.rdword(sp+56); i7=r.rdword(sp+60)
        print("    #%2d sp=0x%08x  ret(i7)=0x%08x  next_fp=0x%08x"%(d,sp,i7,fp),flush=True)
        if fp==0 or fp==sp: break
        sp=fp
unwind(ssp)
r.cmd('D')
