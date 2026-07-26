
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
