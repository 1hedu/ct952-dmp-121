
### 12.68 DSP-model progress — the display state machine (0x5abb4) is only ever sent event 7; it never receives the next event

Attacking the state-7 gate (crutches VDEC_IDLE+VSYNC_KEEP, SKIP_READY OFF). Findings:

- **`0x5af60` (the DISPSTATE stepper) is INSIDE `0x5abb4`** — `0x5abb4(event)` IS the
  display state-machine handler; it records `event` into DISPSTATE `[0x40039949]` and
  toggles READYFLAG bit 0x20 transiently (guarded so `event==0xd` is special-cased, so
  "reach 0xd" was a partial misread — 0xd is a *reset/clear* arg, not a target).
- **`0x5abb4` is only ever called with event `7`** (`CT952_PCWATCH` on 0x5abb4:
  callers `0x5f070`, `0x420a8`, `0x41a90` — all `o0=7`). Events 8..? are never sent.
- **`DISPSTATE` is written `7` once (33.8M) and never changes** (new `CT952_WWATCH`
  taps on `0x40039949`/`0x4003996c`). `READYFLAG` bit 0x20 only *pulses* (set at
  33.6M/47.5M by `0x5a6c8`, cleared ~450K instr later by `0x36ff0`).

**Interpretation:** the display event sequence stalls at event 7. Event 7 correlates
with **source 7** (the recurring internal-source index; F_REQ bit0x80 == source 7,
`0x5abb4(7)`, `0xafd8(7)`). So the machine is parked in "source-7 / internal-source"
handling and never receives the follow-on event that would advance it — and
`0x5abb4(7)` (called from boot-init `0x41a90`) does not return, which is what forced
`SKIP_READY`.

**Open (next focused step):** find who *should* send the next display event after 7
(the follow-on `0x5abb4(N)` for N>7, or the internal advance inside `0x5abb4(7)`) and
what decoder/display condition gates it. That producer — a VSYNC-ISR display step or a
sibling thread — is the exact DSP/display behavior to model so `0x5abb4(7)` completes
and returns naturally. The event-7 stall + source-7 correlation strongly suggest the
missing piece is the **internal-source "ready" signal** the display sequencer waits on
before stepping past event 7. New WWATCH taps: DISPSTATE 0x40039949, READYFLAG 0x4003996c.

### 12.69 CORRECTION + precise location — without SKIP_READY the boot is DEADLOCKED (all threads parked) in 0x5abb4(7)'s blocking mbox-get

Fine-grained PC histogram (`CT952_PCSAMPLE=30000`, VDEC_IDLE+VSYNC_KEEP, SKIP_READY
OFF) past 45M: the hottest PCs are all **eCos idle/scheduler** (`0x4001d7f8/0x4001d804/
0x4001d80c`, `0x40001030-0x400010a8`). No application code runs — every app thread is
**parked**. So the SKIP_READY-off state is a true **deadlock**, not a livelock.

**This corrects two errors I made this turn** (fatigue-driven, flagged honestly):
- `0x5abb4(7)`'s wait is a **blocking, untimed mbox-get** (§12.52), NOT the 24-tick
  `0xa33d8` poll I mis-attributed. The main thread blocks there and never wakes.
- The DISPSTATE "7→0xd sequence" was wrong: `0x5abb4` events are display *commands*
  (1/3/4/6/7/0xa), and 0xd is a special reset arg (§12.68). DISPSTATE is not a linear
  gate.

**Precise deadlock:** main/INITIAL thread → boot-init `0x41a90` → `0x5abb4(7)` →
blocking mbox-get → parks forever. The mbox is posted by the CC-event worker `0x6748`,
which is resumed only *after* `0x5abb4(7)` returns (`0x41b04`, §12.63). Circular:
producer-needs-resume, resume-needs-0x5abb4-return, 0x5abb4-needs-producer. This is the
SAME §12.49-§12.53 missing-mbox-producer wall, now pinned to its exact instruction.

**Where the DSP model actually bites (honest re-frame):** the deadlock is NOT a DSP
register poll (§12.67: no bram reads at the stall). It is the CC-event **mbox producer**
— the same wall the whole quest has circled. The DSP model (VDEC_IDLE) cleared the ONE
decoder handshake that was real; the remaining wall is the software CC-event producer
that is circularly gated. On real silicon `0x5abb4(7)`'s mbox wait IS satisfied (the
frame boots card-less), so SOMETHING posts that mbox before/without the worker — an ISR
or a sibling thread the emulated boot doesn't run. Identifying that poster (which mbox
object `0x5abb4(7)` blocks on, and who posts it on hardware) is the true remaining
keystone — not more DSP-register modeling.

**Honest status:** genuine progress locating the deadlock to one instruction; two
mid-turn model errors caught and corrected. The keystone is the CC-event mbox producer
(software), reachable by tracing which mbox `0x5abb4(7)` blocks on and its poster.

### 12.70 MAJOR CORRECTION — the boot is not deadlocked, it is SLOW (real firmware delays × slow eCos clock); the worker resume is reached NATURALLY once the clock is sped up

A deterministic call-trail from the `0x41a90` entry (`CT952_CALLTRAIL`: log every CALL
after the main thread enters `0x5abb4(7)`) overturned the §12.63/§12.69 "hang" reads
(which were static-analysis errors):

- **`0x5abb4(7)` RETURNS** — execution reaches `0x41aac` right after it. It does NOT
  hang. `SKIP_READY` was bypassing a non-problem.
- **`0x59e90` = `SOURCE_Select(source)`**, called with **source 7 (internal source)**;
  it also RETURNS (at 48.4M, `PCWATCH` on its return `0x41ab4`).
- The boot-init then runs a sequence of real **`OS_DelayTime` calls (`0x59850`)**:
  `0x59850(0x64)` = 100 ms took ~13M instructions (48.4M→61.7M) but **returned**;
  `0x59850(0x5dc)` = 1500 ms would take ~200M instructions. The eCos clock advances
  ~133K instr/ms, so real firmware delays consume enormous instruction counts.

**So the "deadlock" is largely SLOWNESS, not a missing producer.** With the intended
knob `CT952_TICK_FAST_AT=48000000,256` (speed the eCos TIMER1 clock 256× after the
early gates), the boot-init completes the delays and **reaches the worker-resume
`0x41b04` at 54.8M NATURALLY** — no `SKIP_READY`. The resume path is real and reached
on its own once the delays are given a runnable time budget.

**This reframes the whole late-boot picture:** VDEC_IDLE (decoder playmode) + VSYNC_KEEP
(display VSYNC) + a sped-up eCos clock let the boot-init run to completion through its
legitimate delays and reach the CC-event worker resume — the crutches SKIP_READY and
the "missing mbox producer" framing (§12.49-69) were partly artifacts of the boot
being time-starved (delays never completing in the instruction budget of a test run).

**Now testing:** long run with `TICK_FAST_AT=...,512` to see whether, past the resume,
the worker starts and cascades to `POWERONMENU_Initial` → the idle screensaver/
slideshow. New knob: `CT952_CALLTRAIL`. (Correcting my own §12.63/12.69 hang-location
errors, caught by measuring instead of static-reading.)
