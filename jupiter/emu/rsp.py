# Minimal GDB-remote (RSP) client for driving ct952emu's --gdb stub.
# Usage:
#   ./ct952emu --restore pom.snap --gdb 3333 --quiet dp700wd.bin &
#   python3 -c "from rsp import RSP; r=RSP(); print(hex(r.rdword(0x40033830)))"
# regs(): 72-entry list (pc=[68], npc=[69], o6/sp=[14], i6/fp=[30], i7=[31]).
# bp/rmbp/cont/step/rdmem/rdword/reg. A timeout-continue must send b'\x03'
# (Ctrl-C) before the next command or the target keeps running (see 10.49).
import socket,sys
class RSP:
    def __init__(self,port=3333,host='127.0.0.1',timeout=300):
        self.s=socket.create_connection((host,port),timeout=timeout)
    def _cs(self,d): return sum(d.encode())&0xff
    def send(self,body): self.s.sendall(b'$'+body.encode()+b'#'+('%02x'%self._cs(body)).encode())
    def read(self):
        while True:
            c=self.s.recv(1)
            if not c: return None
            if c in (b'+',b'-'): continue
            if c==b'$': break
        buf=b''
        while True:
            c=self.s.recv(1)
            if c==b'#': break
            buf+=c
        self.s.recv(2); self.s.sendall(b'+')
        return buf.decode()
    def cmd(self,body): self.send(body); return self.read()
    def regs(self):
        g=self.cmd('g'); return [int(g[i*8:i*8+8],16) for i in range(len(g)//8)]
    def reg(self,n): return int(self.cmd('p%x'%n),16)
    def rdmem(self,addr,length):
        r=self.cmd('m%x,%x'%(addr,length))
        return bytes(int(r[i:i+2],16) for i in range(0,len(r),2))
    def rdword(self,addr):
        b=self.rdmem(addr,4); return int.from_bytes(b,'big')
    def bp(self,addr): return self.cmd('Z0,%x,4'%addr)
    def rmbp(self,addr): return self.cmd('z0,%x,4'%addr)
    def cont(self): return self.cmd('c')
    def step(self): return self.cmd('s')
REGNAME=(['g%d'%i for i in range(8)]+['o%d'%i for i in range(8)]+
         ['l%d'%i for i in range(8)]+['i%d'%i for i in range(8)])

import select
def cont_to(r, secs):
    """Continue; if no stop within `secs`, send Ctrl-C and read the stop reply.
    Returns (stop_reply, interrupted_bool)."""
    r.send('c')
    ready,_,_=select.select([r.s],[],[],secs)
    if ready:
        return r.read(), False
    r.s.sendall(b'\x03')
    return r.read(), True
