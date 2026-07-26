
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

### 12.71–12.74 (recap from commit log; doc edits didn't land those turns)
- **§12.72** modelled IIC data-ready status `0x8000420c` bit2 (per-byte config-read
  poll was timing out). Commit `fac6f68`.
- **§12.73** with VDEC_IDLE + VSYNC_KEEP + `TICK_FAST_AT=48000000,512` + the IIC fix,
  the boot advances to ~82M then *appeared* to park (later shown to be the periodic
  idle poll, not a park). Commit `ed1116b`. NOTE: the "parks at 0x4003cc00 mbox"
  reading was WRONG — see §12.75; `0x5969c` is `cyg_thread_yield`, not mbox-get, so
  `o0=0x4003cc00` was a stale register, not a mailbox object.
- **§12.74** RAM dump: no removable source present; source-detect handler `0x12b30`
  never runs and is not indirectly dispatched from flash/DRAM. Commit `a74ad97`.

### 12.75 ★ BREAKTHROUGH — the faithful boot DECODES AND RENDERS THE SLIDESHOW PHOTO

The whole quest's acceptance test is met: booting retail `dp700wd.bin` with only the
device-timing knobs (`CT952_VDEC_IDLE=1 CT952_VSYNC_KEEP=1
CT952_TICK_FAST_AT=48000000,512`, and `--skip-panelcfg`), eCos comes alive on its own,
the firmware's DSP/JPU decode path runs, and **built-in demo photographs are decoded
into the slideshow frame buffer and cycle** — a real photo-frame slideshow. No
behavioural crutch is needed for the photo path (`TICK_FAST_AT` only accelerates the
eCos tick's wall-clock; it does not change behaviour).

**How it was found (correcting several prior misreads):**
- The boot is **not deadlocked and not parked** — it runs indefinitely (verified to
  242M+ instr). Threads cycle on an F_REQ/F_DONE event handshake (flag `0x40026e9c`/
  `0x40026ea4`, bit `0x04000000`) every ~13M instr = one ~100 ms idle tick. That is
  normal idle polling, not a stall. (§12.69's "deadlock" and §12.73's "0x4003cc00
  mbox park" were both wrong: `0x5969c` is `cyg_thread_yield` → argless `0x4001de40`,
  not the generic mbox-get, so the traced `o0` values were stale registers.)
- The main app thread runs a **message pump** (`0xa6cc`, entry `0xadb0`) that dequeues
  messages and **indirect-calls** the handler pointer stored in each message
  (`call %o1` @ `0xa720`, `call %o0` @ `0xa778`) — the interprocedural + indirect
  dispatch that made static caller-analysis miss the flow (exactly the user's warning).
  New knob `CT952_ICALL` logs the targets: over 100M instr only **two** handlers ever
  run — `0x261cc` (msgtype 0xa0, the splash/idle tick `0x2747c`) and `0x260f4`
  (msgtype 0x3d, once). `POWERONMENU_Initial` (`0x61be8`) is reachable from 8 handler
  sites but its message type is never posted, so the DVD-style power-on **menu** never
  pops — which is correct for a photo frame that boots straight into the slideshow.
- The eCos flag API in this build: `0x4001dffc`=wait, `0x4001e0b0`=peek,
  `0x4001e02c`=wait(mode), `0x4001e014`=maskbits, wrappers `0x59610/28/40/597d0`.
- **The slideshow photo lives in the video plane, not the OSD plane.** The emulator's
  JPU MCU-BIU model (`machine.c:109`) writes the decoded frame as **macroblock-tiled
  YUV 4:2:0** to `DS_FRAMEBUF_ST_SLIDESHOW` = `0x40065000` (Y) / `0x400B3C00` (C),
  strip `0x2D00` — exactly what the hardware scan-out DAC reads. The log line
  `JPU MCU-BIU wrote 640x360 tiled YUV to 0x40065000/0x400b3c00` fires **repeatedly**
  in a plain baseline run: the slideshow is decoding successive photos.
- The earlier "green horizontal stripes" image was a **rendering-tool artifact**, not
  a firmware/emulation fault: `--fb-out` scans the small OSD plane (`0x4005F000`,
  ~620×78) at 616×440, overrunning into the tiled video FB and rendering those bytes
  as palette indices (index 0 → BT.601 `(0,135,0)` green, machine.c:2427). De-tiling
  the video plane with the documented tile geometry yields a **pristine full photo**
  (Grand Teton / Moulton Barn; a flower+butterfly frame; etc.).

**Emulator change:** added `machine_video_scanout()` + `--video-out PPM [--video-wh
WxH]` (default 640×360) that de-tiles the slideshow plane straight from DRAM
(`0x40065000`/`0x400B3C00`) to RGB — the faithful "what the panel shows" render, no
host-side shortcut. (`videoplane.py` did the same de-tile off a snapshot; this makes it
a first-class emulator output.)

**Diagnostic knobs added this session (sparc.c):** `CT952_BTAT=<pc>[,lo,hi]` +
`CT952_BT_FROM` (register-window backtrace at any PC/sp-window — attributed the main
thread's `OS_DelayTime` park to the splash tick); `CT952_ICALL` (message-pump indirect
dispatch logger); `CT952_FORCE_POM=<icount>` (experiment: redirect one pump dispatch to
the POWERONMENU wrapper `0x2620c` — invokes `POWERONMENU_Initial(1)`; used to probe the
menu path, NOT needed for the slideshow).

**Open (faithful polish, not blockers):**
1. The DVD-style power-on **menu** still isn't posted (splash tick `0x2747c` loops
   while its display-ready flags — READYFLAG `0x4003996c`, `0x40032b4f`, … — never
   reach the advance state). A photo frame boots straight to slideshow, so this may be
   by-design; if the menu/OSD overlay is wanted, model whatever ISR/thread sets those
   flags. `FORCE_POM` enters `POWERONMENU_Initial` but it then blocks in its own menu
   loop (completion store `0x61ca4` not reached).
2. ~~Make `--fb-out` composite the de-tiled video plane under the real-bounds OSD.~~
   **DONE (§12.76):** `machine_disp_scanout` now samples the de-tiled video plane
   straight from DRAM (shared `video_sample_rgb`) wherever the OSD is transparent, and
   CLAMPS the OSD read to the real region (0x4005F000..0x40065000) so it no longer
   overruns into the tiled video buffer. `--fb-out` shows the clean photo + the OSD's
   real content (a thin status strip at rows 7–12) in one scan-out; the green stripes
   are gone. Verified across three distinct cycling slideshow frames (butterfly/
   lantana, Grand-Teton/Moulton-Barn, Tuolumne river) — the built-in `--video-out`
   de-tile and the composite `--fb-out` agree.

### 12.77 ★ The cycling photos ARE the slideshow — no menu transition is the faithful boot path

Resolving the "splash→menu transition" open item (§12.75 #1): there is **no missing
trigger to model** — the frame boots straight into its built-in photo slideshow, which
is exactly what a digital photo frame does.

Evidence (`--jpeg-out` / decode log of a plain baseline boot):
- The firmware's **MM (multimedia) subsystem** stages successive built-in demo JPEGs
  into the MM video buffer `DS_VDBUF_ST_MM = 0x401dc000` (the emulator's decode hook
  fires on each new SOI `FF D8` with a changed signature) and kicks the DSP/JPU decode.
- Decode #1 is a 480×270 boot logo; decodes #2..N are **distinct 640×360 photos**,
  10+ per run — i.e. the slideshow is actively cycling images (butterfly/lantana,
  Grand-Teton/Moulton-Barn, Tuolumne river, …).
- `_bOSDSSScreenSaverMode=0` and `__bOSDSSPicIdx=0`: this is the **MM photo player**
  slideshow, not the OSDSS *screensaver* (OSDSS is the after-longer-idle dimmer that
  arms from the menu; the photo frame's main function is this MM slideshow, and it
  runs on its own).

Why the splash tick `0x2747c` "never advances to POWERONMENU": the DVD-heritage
power-on menu is **not** part of this DMP photo-frame build's auto-boot. The splash
tick's advance is gated by `[0x4002fb48]` (a display-state byte written from 14 sites
across the display subsystem) + READYFLAG `0x4003996c` bit 0x20; it is an emergent
display state, not a single device signal, and the frame simply never drives it to the
"menu" state because it goes to slideshow instead. `FORCE_POM` can shove
`POWERONMENU_Initial` in, but that is DVD behaviour, not the faithful photo-frame path.

**Conclusion:** the acceptance test — retail firmware boots faithfully and reaches the
built-in photo slideshow rendering — is **met**. The remaining faithful nicety is only
that `--fb-out` already composites the panel (§12.76); the menu is a non-goal for this
build. If OSDSS *screensaver* mode is specifically wanted, that is a separate, smaller
follow-up (arm OSDSS from the idle path), but the visible slideshow is already running.

### 12.78 CORRECTION — the cycling slideshow IS the OSDSS JPEG screen saver (§12.77 was wrong)

§12.77's claim "this is the MM photo player, NOT the OSDSS screensaver" was **wrong**,
and it rested on two unreliable sources I should not have trusted:
- the `_bOSDSSScreenSaverMode` address `0x400239c4` from main.c's DUMPFLAGS list, which I
  never verified from the binary (only `__bPOWERONMENUInitial=0x40023a10` was verified,
  from POWERONMENU_Initial's own `stb` at `0x61ca4`), and
- relative addresses read off `DVD909.sym` — a **909-family** build, not our CT952A
  `dp700wd.bin`; its data/text offsets don't line up (confirmed).

Grounded in our binary + `950_Files/`:
- The photo decodes are kicked through `UTL_ShowJPEG_Slide` (utl.c:377), which is exactly
  the primitive the **OSDSS screen saver** uses to advance pictures — `_OSDSS_PictureUpdate`
  calls `UTL_ShowJPEG_Slide(JPEG_PARSE_TYPE_NORMAL, …)` (osdss.c:211) — writing the
  decoded frame into `DS_FRAMEBUF_ST_SLIDESHOW` (0x40065000). Empirical decode call chain
  (PCWATCH on the JPU-kick 0x6c500, save-aware caller walk): JPU-kick `0x6c500` ←
  `0x6cea0` ← `0x365f4` ← `0x376b4` ← `0x1b800` ← [indirect-dispatched handler `0x1f5c0`].
- So the built-in demo photos cycling into the SLIDESHOW frame buffer are the OSDSS JPEG
  screen-saver slideshow. `UTL_ShowJPEG_Slide` is shared (OSDSS, thumbnails, logo path),
  so identifying which caller is active is what still needs a clean check — but the
  screen-saver slideshow is the correct name for the observed behaviour, per the target
  owner. My "not OSDSS" was unfounded.

Follow-up to nail down cleanly (from our binary only, no 909): verify the real
`_bOSDSSScreenSaverMode`/`__bOSDSSPicIdx` addresses (via a function that writes them —
OSDSS_Entry sets the mode byte TRUE right before `_OSDSS_PictureUpdate`), instead of the
DUMPFLAGS guesses.

### 12.79 Verified from our binary: an indexed picture slideshow cycles the demo photos (DUMPFLAGS OSDSS addresses disproven)

Trying to pin the real `_bOSDSSScreenSaverMode`/`__bOSDSSPicIdx` addresses (no 909,
no DUMPFLAGS guesses). Tools: `sparc_win_backtrace()` (walks the CPU register windows
for a reliable runtime call stack even when frames aren't spilled) + `CT952_PICIDX`
(diffs the data region between successive photo decodes).

**True runtime decode call stack (register windows) for the cycling photos:**
`[thread] → 0x2747c (display tick) → 0x27d20 (24-state jump-table machine, state var
0x40032b4f) → 0x376b4 (JPEG display) → 0x6c500 (HALJPEG/JPU kick)`. The message pump
(handler 0x261cc, msgtype 0xa0) drives 0x2747c; the OSDSS trigger is asynchronous
(posts the state/message and returns), so it is not on the synchronous stack — which is
why a plain backtrace can't name it.

**Per-decode data diff (CT952_PICIDX) — verified counters:**
- `0x40022fa3`: 1,2,3,4,5,6,7,8,9… — a **monotonic total-decode counter** (NOT a pic
  index).
- `0x40031ad3` / `0x40039935` / `0x400399b7`: 0,1,2,3,4,5 then **wrap** (decode #7 → 1),
  moving in lockstep — a **picture index that cycles through the ~6 built-in demo
  photos**. This is exactly `__bOSDSSPicIdx` behaviour: an indexed photo slideshow.

**DUMPFLAGS addresses disproven:** the guessed `__bOSDSSPicIdx = 0x400239cc` does NOT
cycle (it only reset 255→0 once at boot), so it is not the picture index. Only
`__bPOWERONMENUInitial = 0x40023a10` was ever verified (from POWERONMENU_Initial's own
`stb` at 0x61ca4). The `_bOSDSSScreenSaverMode = 0x400239c4` guess is therefore also
untrustworthy — my earlier "screen-saver mode = 0 → not OSDSS" (§12.77) had no basis.

**Honest limit:** without a symbol map for *this* binary I could not uniquely label the
exact symbols — the three mirrored pic-index candidates are struct-pointer accessed
(defeating the sethi+store scan that pinned `__bPOWERONMENUInitial`), and ~90 booleans
flip at the slideshow transition, so `_bOSDSSScreenSaverMode` can't be singled out by
the dynamic heuristics alone. What IS established from our binary: the demo photos are
driven by a genuine **cycling picture index** (an indexed slideshow), consistent with
the OSDSS JPEG screen saver. Pinning the exact `_bOSDSSScreenSaverMode`/`__bOSDSSPicIdx`
symbols cleanly would need a linker map or a targeted disassembly of OSDSS_Entry, which
I have not located with certainty yet.

### 12.80 ★ VERIFIED OSDSS addresses — and the honest verdict: OSDSS_Entry never runs in our boot (so the current slideshow is PRE-OSDSS)

Careful disassembly (anchored on the verified `__bPOWERONMENUInitial=0x40023a10`, whose
readers include `OSDSS_Monitor`) positively identified the OSDSS code and pinned every
address from our own binary:

| symbol | address | how verified |
|---|---|---|
| `OSDSS_Monitor` | `0x591b4` | reads `__bPOWERONMENUInitial`; matches osdss.c:298 line-by-line |
| `OSDSS_Entry` | `0x59108` | `stb 1,[0x400239c4]` then `OSD_ChangeUI(12,0)` then `clrh [0x400239cc]` |
| `_bOSDSSScreenSaverMode` | `0x400239c4` | the `stb 1` target in OSDSS_Entry (`=TRUE`) — **DUMPFLAGS was RIGHT** |
| `__bOSDSSPicIdx` | `0x400239cc` | the `clrh` target in OSDSS_Entry (`=0`) — **DUMPFLAGS was RIGHT** |
| `__dwOSDSSCheckTime` | `0x400239b8` | OSDSS_Monitor `ld/st` |
| `__dwOSDSSCheckNOData` | `0x400239c0` | OSDSS_Monitor |
| `OSD_ChangeUI` | `0x4a754` | called `(12,0)` from OSDSS_Entry |
| `OSDSS_ENTER_TIME` | `0xe260` (~58 s) | the idle threshold OSDSS_Monitor compares against |

**This overturns §12.78/§12.79 and RESTORES §12.77.** The DUMPFLAGS addresses were
correct all along; my §12.78 concession ("the guessed addrs are unreliable, so it must
be OSDSS") and §12.79 ("0x400239cc isn't the pic index") were both wrong — 0x400239cc
IS `__bOSDSSPicIdx`, it just never cycles because OSDSS never enters.

**OSDSS_Entry is statically single-caller and gated:** its only caller is OSDSS_Monitor,
which calls it only when `__bPOWERONMENUInitial != 0 && __bCLOCKShowClock==0 &&
__bAlarmState==0` AND the idle time exceeds ~58 s. Empirically (PCWATCH + OSDSSWATCH on
the verified addresses, run to 120M): **OSDSS_Monitor executes 40+ times but
OSDSS_Entry executes 0 times**, and `_bOSDSSScreenSaverMode` is never set to 1. The gate
that fails is `__bPOWERONMENUInitial == 0` (POWERONMENU_Initial never completes, §12.75).

**Honest verdict on "is this the OSDSS screensaver?":** On the *real device*, the photo
screen saver IS OSDSS — the user is right about the product. But in *our current
emulated boot*, the cycling JPEG photos are NOT the OSDSS path: they are a pre-OSDSS
media/demo slideshow driven by the display state machine `0x27d20` (item index at
`0x40031ad3`, a struct field — distinct from `__bOSDSSPicIdx=0x400239cc`, which stays 0).
Reaching genuine OSDSS requires `__bPOWERONMENUInitial=1` (POWERONMENU_Initial to run to
its store at 0x61ca4) followed by ~58 s idle — then OSDSS_Monitor calls OSDSS_Entry and
`_bOSDSSScreenSaverMode` flips to 1. That is the precise remaining step to the *actual*
screen saver, and it ties back to the POWERONMENU-completion gate.

### 12.81 Reached genuine OSDSS (OSDSS_Entry runs, _bOSDSSScreenSaverMode=1) — and the full gating chain to it

Chased POWERONMENU→OSDSS. Findings (all from our binary):

**POWERONMENU is event/key-triggered in this build, not an auto-boot call.** All 8
call sites of POWERONMENU_Initial(0x61be8) are tiny wrapper functions (`save; call
0x61be8`) reached via the message pump / key-command dispatch — none is a big
Thread_CTKDVD that calls it synchronously. `__bPOWERONMENUInitial(0x40023a10)` is set to
1 ONLY by POWERONMENU_Initial's own store at 0x61ca4 (the only other writer, 0x61894,
clears it). So `__bPOWERONMENUInitial` stays 0 until a menu/stop key drives POWERONMENU.
Under FORCE_POM, POWERONMENU_Initial runs on the message-pump thread and calls
SOURCE_Select(0x59e90)+OSD_ChangeUI(0x4a754) before its store, and stalls there
(cross-thread), so it doesn't complete.

**OSDSS_Monitor's full gate to OSDSS_Entry** (verified by PCWATCH on the internal PCs):
`__btPowerDown==0` → `__dwOSDSSCheckTime!=-1` → `!_bOSDSSScreenSaverMode` →
`__dwOSDSSCheckNOData==__dwTimeNow` (reached, 40×) → `idle=OS_GetSysTimer()-
__dwOSDSSCheckTime > 0xe260 (~58s)` (NEVER true) → `__bPOWERONMENUInitial!=0` →
`__bCLOCKShowClock==0` → `__bAlarmState==0` → `OSDSS_Entry(0x59108)`.

**Why idle never elapses:** measured at the compare (0x59244), the idle climbs only to
~65 ms and is periodically reset by `0x59424` (an OSDSS timer-reset, called from the
key-command handler 0x3a38 and OSDSS internals). The device is *actively running the
demo attract slideshow*, which is legitimate activity — so OSDSS (the IDLE screen
saver) correctly does not arm while the demo plays. At the emulated clock rate reaching
58 s of true idle would need ~250M uninterrupted instructions.

**Forced entry (CT952_SET_POM + CT952_FORCE_OSDSS):** set `__bPOWERONMENUInitial=1` and
make the idle compare see an elapsed value; OSDSS_Monitor then called `OSDSS_Entry` on
its own (PCWATCH: 0x59108 fires once), and `_bOSDSSScreenSaverMode` flips to **1** at
0x59138 — the genuine OSDSS screen-saver state. `osdss_screensaver.png` is the resulting
render. Visually identical to the demo (same built-in photos, as expected — OSDSS uses
the same UTL_ShowJPEG_Slide path), but now it is the actual OSDSS code path.

**Faithful (non-crutch) route to OSDSS**, now precisely known: inject a menu/stop key
(CT952_IRKEY drives the real IR ISR → CC key command) so POWERONMENU_Initial runs to
its 0x61ca4 store (`__bPOWERONMENUInitial=1`); stop the demo attract slideshow (so the
idle timer stops being reset); let ~58 s of eCos time elapse → OSDSS_Monitor calls
OSDSS_Entry naturally. Two crutches (SET_POM, FORCE_OSDSS) stand in for the first and
second of these; the third is just time.

### 12.82 Input injection confirmed — IR (end-to-end) and panel SAR/ADC (channel-aware)

Confirmed both faithful input paths drive the firmware from our binary.

**IR remote (CT952_IRKEY) — end-to-end.** `CT952_IRKEY=0x0c@43000000` presents a NEC
frame to the modeled IR receiver and raises the PROC1-2nd IR interrupt (cascades to LEON
line 10, which is in the mask). ~1400 instructions later `__bISRKey(0x40039074)` was
written to **0xc8** at pc 0x423a0 — i.e. the firmware's own IR ISR decoded scancode 0x0c
to `KEY_EXIT` (INPUT_KEY_GROUP11+0 = 200 = 0xc8). The whole receiver→ISR→__bISRKey path
works.

**Panel key ladder (CT952_PANELKEY) — channel-aware ADC.** `PANEL_KeyScan` (flash
**0x5988c**, called from ~60M by the input thread, 40×/run via caller 0xa3b4) selects a
ladder line by writing ADCGLB[23:16] (0x84 → RDATA0 line, 0xC4 → RDATA1 line) then reads
`0x8000407C`; `RDATA = read>>24`. `<0xF0` = a key, with thresholds 0xD0/0x90/0x60/0x10
mapping to `aScanMap[1..10]` (key.h: 1=KEY_LEFT, 2=RIGHT, 3=DOWN, 4=UP, 6=PLAY_PAUSE,
8=POWER). The old CT952_ADC returned the same value on both lines (no real key matches
that). New `CT952_PANELKEY="<r0>,<r1>"` reads the channel-select bits and returns r0 only
on the 0x84 line, r1 only on 0xC4, else the 0xFF idle rail. Verified: `CT952_PANELKEY=
0xE0,0xFF` → dump shows `RDATA0(0x40039904)=0xE0` (channel A = injected) and the channel-B
read = `0xFF` (idle) → bGetKey=1 → `KEY_LEFT`. The voltage reaches PANEL_KeyScan and
decodes to a real key. (A held level does not create a fresh press-edge, so the input
debounce doesn't latch __bISRKey from a constant CT952_PANELKEY; a real "press" is a
transition — inject then clear.)

Both are the faithful hardware input paths (real ISR / real ADC scan), usable to drive
POWERONMENU (menu/stop key) and thence the OSDSS screen saver (§12.81).

### 12.83 Input injection "wired up": edge-based, both paths deliver real decoded keys

Made both injection paths produce real press EDGES and traced them all the way in.

**IR (CT952_IRKEY=<scancode>[@<icount>]) — full path, edge via interrupt.** The IR ISR
decode is at flash 0x42370 → scancode→key lookup 0x4241c (NEC decode of IR regs
0x80000390/0x394, customer check at 0x400235ae) → writes __bISRKey(0x40039074) at 0x4239c.
Injecting scancode 0x0c: [KEYwr] 40039074=c8 pc=0x423a0 then [KEYrd] pc=0x423f0 ~6 instr
later — the ISR decodes KEY_EXIT (0xc8) and reads it back. Confirmed.

**Panel (CT952_PANELKEY="<r0>,<r1>[@<at>[,<len>]]") — channel-aware + timed edge.**
Returns r0 only on the 0x84 ladder line, r1 on 0xC4, else the 0xFF idle rail; with @at,len
it presses at `at` for `len` instructions then releases (a real idle→press→release edge,
since the debounce SM 0x59c8c needs a transition). PANEL_KeyScan(0x5988c) is driven by the
input handler at 0xa388 (calls it, then the debounce 0x59c8c). Verified 0xE0,0xFF →
RDATA0=0xE0 → KEY_LEFT decoded.

**Honest gap — action dispatch is state-specific.** An injected key lands in __bISRKey
(IR) or is decoded by PANEL_KeyScan (panel), but does NOT fire POWERONMENU in the current
demo/slideshow state: injecting KEY_EXIT set __bISRKey=0xc8 yet POWERONMENU_Initial
(0x61be8) stayed at 0 hits, and __bISRKey is read only once (by the ISR) in the window —
the demo's active loop isn't polling __bISRKey. So which key+state routes to POWERONMENU
(and thus faithfully to OSDSS) is the remaining piece; the injection mechanisms themselves
are wired and confirmed at the hardware/decode level.

### 12.84 CORRECTION — the running pump DOES poll keys (real key var is 0x400235ac, not 0x40039074)

Prompted by the observation that on real hardware every key acts during the slideshow,
re-examined the key path — and found I had been watching the WRONG variable.

- The IR ISR (0x42370) writes the decoded key to **three** places: `0x40039074`,
  `0x400235ac`, and `0x400235ad` (with timestamps 0x400235b4/b8). `0x40039074` is a
  secondary copy that only the ISR touches — which is why "no app reads __bISRKey"
  (§12.83) looked like the control loop was dead. It is not.
- The **real current-key variable is `0x400235ac`**, and it IS polled by the RUNNING
  message pump: `0xa2c4` (pump's first call each loop) reads it at 0xa2cc and handles
  POWER/LCD (0x51/0x50); `0x9e58` reads it as the idle/no-key gate. PANEL_KeyScan
  (0x5988c) and the pump's key reads both begin at ~60M (the input subsystem comes
  online there). So keys DO reach the control loop — my §12.83 "control loop not
  running" was a measurement artifact of watching 0x40039074.

Still open: injecting KEY_EXIT (0xc8) in the active window (>60M) is polled by the pump
but produces no visible action / no POWERONMENU — `0xa2c4` only acts on POWER/LCD, and
where a *general* key (navigation/menu) is dispatched in the slideshow state is not yet
pinned. New knob: CT952_KEYWATCH=<from> (distinct-PC read watch on 0x400235ac/0x40039074).
So the input plumbing is faithful (keys are polled); demonstrating a specific key->action
in the slideshow is the remaining piece.

### 12.85 ★ PINNED — general keys aren't dispatched because the active pump handler is the display tick, not an interactive UI

Windowed read-trace of the IR key var `0x400235ac` right after injecting KEY_EXIT (0xc8)
at 63M (`CT952_KEYWATCH`): in the whole window it is read at **exactly one PC, 0xa2d0**
(49x) — the pump's power-key filter `0xa2c4` — and NOWHERE else. `0xa2c4` only acts on
POWER(0x51)/LCD(0x50) and passes everything else through without dispatching or even
clearing it. So a general key (navigation / menu / exit) sits in `0x400235ac`, gets
polled only by the power filter, and is never routed to a handler.

**Why:** the message pump (0xa6cc) only ever runs handler **msgtype 0xa0** (the display/
slideshow tick 0x2747c) and 0x3d (§12.75). The general-key dispatch lives in the
INTERACTIVE UI state handlers (the POWERONMENU wrapper 0x2620c, the MM-UI handlers, etc.)
— none of which is the active pump state. So what renders is the **display/attract path**,
not the interactive MM-UI slideshow that consumes keys. That is exactly why "keys do
nothing in the slideshow," and it is the SAME root as POWERONMENU-never-reached (§12.75,
§12.81): the boot never advances the pump to an interactive UI state.

**Consequence for faithfulness:** the photo rendering is real, but the running state is
attract/display-only; a faithful boot must advance to the interactive UI (POWERONMENU or
MM-UI as the active pump handler), after which general keys dispatch and — via the menu
idle path — OSDSS arms. The blocker is unchanged: advancing the pump past the display
tick to an interactive UI handler. Input injection (§12.82-84) is confirmed to deliver
keys to the key vars; the missing half is the interactive consumer being the active state.

### 12.86 ★ CORRECTED MODEL — the boot settles in pump mode-8 = MEDIA_SELECT_DLG (a real power-on dialog), never entering POWERONMENU_Initial; keys DO reach the pump but are swallowed there

This turn re-derived the late-boot state with clean PC-based tracers (not the stale-register
reads that produced earlier misreads), and corrected two substantive errors in the prior UI
model.

**Corrected UI numbering (ground truth `osd.h`), fixing §12.77/§12.85 labels:**
- pump/OSD **UI 8 = `OSD_UI_MEDIA_SELECT_DLG`** — the power-on *media-source select dialog*,
  NOT "attract". Its 5-entry "source chain" (§12.40, `mode8_stayflag` 0x40032b3b built by
  0x299c0) is the list of selectable media sources; predicate 0x272a8 stays in mode-8 while
  that count > 0.
- **UI 17 (0x11) = `OSD_UI_POWERON_MENU`**, **UI 12 (0xc) = `OSD_UI_SCREEN_SAVER`** (OSDSS),
  **UI 18 (0x12) = `OSD_UI_COPY_DELETE_DLG`** (NOT "MM interactive slideshow" — that summary
  label was wrong).

**Two distinct "ChangeUI" primitives — don't conflate:**
- `0xafd8(mode,sub)` = LOW-LEVEL pump active-mode set. Non-blocking: looks up the mode's
  record (`0xae50`), calls its enter-handler `[rec+4]`, and on nonzero stores the record ptr
  to the active-UI slot `0x40020ec8` (`st %i0,[%l0+0x2c8]` at 0xb038 — this is the
  "activeUI -> rec=40024ce0 id=8" trace). Returns.
- `0x4a754` = HIGH-LEVEL `OSD_ChangeUI(ui,mode)` (the C API POWERONMENU_Initial calls).

**Verified facts (full recipe `VDEC_IDLE=1 VSYNC_KEEP=1 TICK_FAST_AT=48000000,512
--skip-panelcfg`, runs to 260M):**
1. **`OSD_ChangeUI` (0x4a754) is NEVER called** (new `CT952_UICHANGE` PC-tracer, 0 hits in
   260M). The high-level UI genuinely never transitions.
2. **`POWERONMENU_Initial()` is never ENTERED.** It would call `OSD_ChangeUI(17)` (step 4)
   AND set `__bPOWERONMENUInitial=1` (step 8, watch on 0x40023a10) — neither happens. So
   `Thread_CTKDVD` (cc.c:1283: `INITIAL_System → INITIAL_PowerONStatus → POWERONMENU_Initial`)
   **stops before the POWERONMENU_Initial call.** Build config confirms it is compiled in
   (`SUPPORT_POWERON_MENU` defined, `SUPPORT_PLAY_MEDIA_DIRECTLY_POWER_ON` off, `Winav.h`).
3. **The boot-init tail 0x41b00-0x41b58 calls `0xafd8(8)` (set MEDIA_SELECT), which returns**
   (mode-8 record activated at 60.7M). So the mode-set is NOT the block; the block is later
   on Thread_CTKDVD's path to POWERONMENU_Initial — consistent with a media-select wait that
   never resolves because no media source is modeled present.
4. **Without the recipe knobs the pump 0xa6cc never runs at all** (0xa720/0xa778 dispatch =
   0 hits to 50M via `CT952_ICALL`); the steady state is the eCos **idle thread** (tight loop
   0x40001014-0x40001060 + window traps at 0x40000060), waking only to decode the next demo
   photo. This corrects the prior "pump loops the display tick" framing for the no-knob case.
5. **Steady state (with knobs): pump event var `0x40020ec8` alternates `0xa0` (idle) and
   `0x3d` (the ~13M-period slideshow-advance event) forever** (`CT952_PUMPARG`/`PUMPREC`).
   The demo slideshow cycling IS this 0x3d event; it is a display/timer path, independent of
   the parked Thread_CTKDVD.
6. **Keys DO reach the pump.** Injecting IR scancode 0x0c at 65,002,861 posts pump event
   **`0xc8` at 65,004,477** (~1600 instr later) into `0x40020ec8`, then it clears back to
   `0xa0`. So the key is received by the message pump — but in mode-8 (media-select) it is
   processed as a no-op and produces no UI transition. (Refines §12.85: keys are not merely
   unrouted; they are delivered to the pump and swallowed by the media-select state.)

**Unified faithful gap (unchanged root, now precisely characterized):** `Thread_CTKDVD` sets
pump mode-8 (MEDIA_SELECT_DLG) and blocks before `POWERONMENU_Initial()`, waiting on a
media-source resolution that never comes (no card/USB/internal source is modeled present).
The demo photos we render are mode-8's background slideshow. The faithful event to model is
**media-source presence/selection** so the media-select resolves, Thread_CTKDVD returns and
calls `POWERONMENU_Initial()` → `OSD_ChangeUI(17)` → the interactive menu, where general keys
(already delivered to the pump, §12.86.6) navigate and the menu idle path arms OSDSS.

**New diagnostics this turn (all opt-in, inert unless env-set):** `CT952_UICHANGE` (PC-trace
OSD_ChangeUI 0x4a754), `CT952_PUMPARG[=<icount>]` (mode-handler dispatch arg at 0xa720 +
message-record fetch at 0xa6ec), `CT952_POKE` (one-shot DRAM write, for gate experiments).

### 12.87 ★ THE EXACT WAIT PINNED — the INITIAL thread enters MEDIA_SELECT (mode-8) SUCCESSFULLY and returns, skipping the POWERONMENU fallback; POWERONMENU_Initial is never invoked by any thread

Followed "pin the exact wait" to the single deciding branch, with runtime confirmation.

**The boot's power-on thread is flash function `0x418f0`** — a thread ENTRY (backtrace at
its body shows the caller chain is the eCos thread trampoline `0x4001ea40/0x4001ea60`, not a
flash caller). It runs the power-on sequence (readiness `0x5abb4(7)`, `SOURCE_Select`
`0x59e90`, `OS_DelayTime`s, worker-resume `0x66dc`) and ends with a two-way UI choice at
`0x41b30-0x41b58`:

```
41b30  mov 8,%o0
41b34  call 0xafd8      ; enter MEDIA_SELECT_DLG (mode 8)
41b3c  and %o0,0xff,%o0
41b40  cmp %o0,0
41b44  bne 0x41b58      ; <-- THE DECISION: if mode-8 entered OK, RETURN
41b48  mov 7,%o0        ; else fall through to:
41b50  call 0xafd8      ; enter mode 7 (whose enter-handler calls POWERONMENU_Initial)
41b58  ret              ; thread returns -> exits
```

**Runtime pin (`CT952_PCWATCH`, full recipe):**
- `0x41b34` hit at 55.88M (`o0=8`).
- `0x41b44` hit at 60.686M with **`o0=1`** — i.e. `0xafd8(8)` **returned SUCCESS**.
- `0x41b58` (ret) hit at 60.686M; **`0x41b50` (the mode-7/POWERONMENU fallback) is NEVER
  reached.**
- **`POWERONMENU_Initial` (0x61be8) is never entered by any thread** — 0 hits across ALL its
  call sites (`0x2620c/0x26210, 0x26444, 0x265e4, 0x26de8, 0x1f610, 0x67d68`) in 100M
  (`CT952_PCWATCH`). Its callers are per-mode ENTER-handlers (`save; call 0x61be8(1)`),
  reached only when the pump enters mode-7-family via `0xafd8`; that entry never happens.

**Why `0xafd8(8)` succeeds:** mode-8's enter-handler `[rec+4] = 0x25ef4` returns nonzero
because the media-select source list has **5 CONFIGURED sources** (built unconditionally by
`0x299c0`; count stored in `mode8_stayflag` 0x40032b3b; §12.40). Configured != present media,
so it enters the dialog regardless of whether any card/USB is inserted. `0xafd8(8)` also does
real work (runs the mode-8 enter path incl. the demo-JPEG decode) — hence the ~4.8M-instr gap
between 0x41b34 and its return.

**So the "exact wait" is not a blocked eCos primitive — it is a taken branch.** The device's
faithful power-on UI IS the **MEDIA_SELECT_DLG (mode 8)** — the "menu with a timeout" the
photo frame shows — and the INITIAL thread deliberately returns into it and exits.
POWERONMENU is only the *fallback* for the no-configured-source case, which this build never
hits. Pinned OSD_ChangeUI address for POWERONMENU_Initial's own body: it calls
`OSD_ChangeUI(17)` at `0x61c94` and sets `__bPOWERONMENUInitial=1` at `0x61ca4` (guard
`if(flag) return` at 0x61bf0), matching poweronmenu.c:380 exactly.

**Consequence for the faithful gap (keys / menu):** mode-8 is the correct terminal UI. What
is missing is its INTERACTIVITY + resolution: (a) key events must be dispatched to the mode-8
message handler `0x260f4` (its `[rec+8]`) so a key navigates/selects a source — an injected
IR key posts event `0xc8` into the pump slot 0x40020ec8 (§12.86.6) but the pump does not route
it to `0x260f4`; and (b) a source SELECTION or auto-select TIMEOUT must fire to leave the
dialog (its tick handler `0x261cc`→`0x2747c` has no timeout logic). Modeling media-select
resolution — the dialog consuming keys and/or a selection/timeout — is the faithful lever, NOT
forcing the source count to 0 (§12.40's decline reaches POWERONMENU but is unfaithful: the
real device has configured sources and legitimately shows media-select). Next: trace the
mode-8 key path (`0x260f4` → `0x22494`) and the dialog's selection/timeout to name the exact
resolution event to model.

### 12.88 ★ MAJOR REFRAME — keys ARE dispatched and handled by an interactive photo-browser; the slideshow auto-advances via KEY_NEXT; "keys do nothing" was a KEY_EXIT + media-not-ready artifact

Traced the mode-8 key path end-to-end and CONFIRMED at runtime, overturning §12.85/§12.86.6's
"keys are swallowed / unrouted" reading.

**The full key path (runtime-confirmed, `CT952_PCWATCH` + injected IR key):**
`pump 0xa720 → 0x260f4 (active-UI msg handler) → 0xb1ac (key translate) → 0x9fc8 → 0x22494
(action) → 0x22fbc → 0x2ab50`. `0x2ab50` is a **176-entry jump table** (`jmp [0x2b078 +
(key-0x32)*4]`) — the photo-browser / media-manager KEY dispatcher.

**The dispatcher acts on ~24 keys** (non-default table slots), incl. the full nav set:
- KEY_UP/DOWN/LEFT/RIGHT `0x8d-0x90` → `0x2ace4`; KEY_MENU `0xb5` → `0x2abb8`;
  KEY_NEXT `0x3d` → `0x2ae38`; KEY_PREV `0x3e` → `0x2af4c`; KEY_BROWSE `0xe0` → `0x2adfc`;
  KEY_COPY_DEL `0xe1` → `0x2ae18` → `0x233cc` (→ OSD_ChangeUI(COPY_DELETE_DLG=18); this is
  what the earlier "UI 0x12 transition" actually is — a copy/delete key, not a media resolve).
- ~150 other codes (incl. **KEY_EXIT `0xc8` = GROUP11+0**) map to the DEFAULT no-op `0x2b06c`.

**Why the earlier test misled:** the injected IR scancode 0x0c decodes to key **`0xc8` =
KEY_EXIT**, which is a deliberate **no-op in this UI** — so "the key did nothing" was correct
but unrepresentative. The dispatcher itself runs fine (PCWATCH: `0x260f4`→`0x22494` fire on
the injected key AND on the periodic event).

**The "periodic 0x3d event" is KEY_NEXT** (`GROUP4+1 = 61 = 0x3d`): the slideshow AUTO-ADVANCE
posts KEY_NEXT ~every 13M instr through the SAME dispatcher (`0x260f4→0x22494→0x2ab50→0x2ae38`).
So the demo slideshow IS the interactive photo browser auto-playing — not a separate attract
path. `0x40020ec8` alternating `0xa0`/`0x3d` (§12.86.5) is idle-tick vs KEY_NEXT.

**Why keys still don't visibly navigate (the real remaining gap):** the key handlers GATE their
actions on media/browser-READY state and bail to no-op (`return 0xa2`) when not ready. The nav
handler `0x2ace4` checks `0x4003996c` (READYFLAG), `0x40032b18`, `0x40032b04`, `0x40032afc`;
KEY_NEXT `0x2ae38` checks `0x4003996c`/`0x40032b04`; the COPY_DEL resolve `0x233cc` needs
`0x4003996c & 0x20` AND source `0x40032b4f == 8`. `0x4003996c` bit 0x20 (READYFLAG) only
**pulses** (set by `0x5a6c8`, cleared by `0x36ff0`; §12.68) because no real media source is
present to hold it — so at the moment a key arrives the browser is "not ready" and the handler
no-ops. With no browsable media there is also nothing to navigate to.

**So the faithful gap is narrowed and named:** it is NOT key routing (that works) — it is the
**media/browser READY state**. The lever is to model a media source as PRESENT and ready so the
READY flags hold: primarily **`0x4003996c` bit `0x20` (READYFLAG)** plus the media-state bytes
the handlers check (`0x40032b04`, `0x40032b18`, `0x40032afc`, source `0x40032b4f`). With a
ready source, nav/menu/browse keys produce real navigation and the browser is interactive.
(The OSD overlay for the menu/cursor may additionally need compositing in `machine_disp_scanout`
to be visible — to verify once a source is ready.)

**Confidence:** HIGH that keys are dispatched & handled (runtime-confirmed path + jump-table);
HIGH that KEY_EXIT is a no-op and KEY_NEXT drives the slideshow; MEDIUM-HIGH that READYFLAG +
media-state is the enabling gate (static reads of the handler bodies; next step is to hold
those flags and confirm nav becomes live).
