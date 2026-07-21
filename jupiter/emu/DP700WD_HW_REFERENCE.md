
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
