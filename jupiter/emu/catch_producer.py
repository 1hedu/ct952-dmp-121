import sys, time
from rsp import RSP, cont_to, REGNAME
r = RSP(port=3333, timeout=300)

def dram(a): return r.rdword(a)

print("=== state at snapshot ===", flush=True)
g = r.regs()
print("pc=%08x npc=%08x sp=%08x fp=%08x i7=%08x icount@snap=75M" % (
    g[68], g[69], g[14], g[30], g[31]), flush=True)
print("__bPOWERONMENUInitial 0x40023a10 =", hex(dram(0x40023a10) >> 24), flush=True)
print("_bOSDSSScreenSaverMode 0x400239c4 =", hex(dram(0x400239c4) >> 24), flush=True)
print("mbox 0x40033830 [+3c signaled] =", hex(dram(0x4003386c)), flush=True)
print("event_flag 0x40026ea4 =", hex(dram(0x40026ea4)), flush=True)

# Breakpoints: PostEvent (event production), flag-setter stb, event-bit80 dispatch
BPS = {0x12f10: "PostEvent", 0x61d30: "flag-setter __bPOWERONMENUInitial=1",
       0x6eec: "EvtDispatch_bit80", 0x12f88: "PostEvent+tail"}
for a in BPS: r.bp(a)
print("=== breakpoints set:", {hex(a): n for a, n in BPS.items()}, flush=True)

hits = {}
t0 = time.time()
BUDGET = 90     # seconds total
rounds = 0
while time.time() - t0 < BUDGET:
    rep, interrupted = cont_to(r, 20)
    rounds += 1
    if interrupted:
        g = r.regs()
        print("  [timeout] no bp in 20s; pc=%08x (still cycling)" % g[68], flush=True)
        continue
    g = r.regs()
    pc = g[68]
    name = BPS.get(pc, "??")
    hits[pc] = hits.get(pc, 0) + 1
    if hits[pc] <= 4:
        print("  *** HIT %08x (%s)  caller i7=%08x o7=%08x sp=%08x" % (
            pc, name, g[31], g[15], g[14]), flush=True)
    # step off the bp and continue
    r.rmbp(pc); r.step(); r.bp(pc)

print("=== summary: hits =", {hex(k): v for k, v in hits.items()}, "rounds=", rounds, flush=True)
r.cmd('D')
