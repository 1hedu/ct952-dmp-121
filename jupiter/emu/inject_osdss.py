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
print("before: ScreenSaverMode(0x400239c4)=0x%x  PicIdx(0x400239cc)=0x%x"%(
      r.rdmem(0x400239c4,1)[0], r.rdword(0x400239cc)),flush=True)
# breakpoints along the screensaver playback path
for a in (0x59004,   # _OSDSS_PictureUpdate
          0x61170,   # UTL_ShowJPEG_Slide (the decode!)
          0x407ffff0):# sentinel return
    r.bp(a)
# inject OSDSS_Entry(0x59108); it uses globals, arg %o0=0
setr(15,0x407ffff0)     # o7 sentinel
setr(8,0)               # o0
setr(68,0x59108); setr(69,0x5910c)  # pc,npc
print("injected OSDSS_Entry(); continuing...",flush=True)
names={0x59004:'_OSDSS_PictureUpdate',0x61170:'*** UTL_ShowJPEG_Slide (SCREENSAVER DECODE) ***',
       0x407ffff0:'returned (sentinel)'}
for i in range(8):
    rep=contb(20)
    if rep is None: print("  [%d] no bp in 20s pc=0x%08x"%(i,r.regs()[68]),flush=True); break
    pc=r.regs()[68]
    print("  [%d] %s (pc=0x%08x)"%(i,names.get(pc,'0x%08x'%pc),pc),flush=True)
    print("      ScreenSaverMode=0x%x PicIdx=0x%x"%(r.rdmem(0x400239c4,1)[0],r.rdword(0x400239cc)),flush=True)
    if pc==0x61170:
        print("  >>> SCREENSAVER IS DECODING A PHOTO <<<",flush=True)
        # let the decode complete a bit then stop
        rep=contb(10);
        break
r.cmd('D')
