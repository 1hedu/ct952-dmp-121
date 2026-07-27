
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

### 12.89 ★★ RESOLVED — the slideshow IS interactively faithful: KEY_NEXT advances the photo, KEY_STOP halts auto-advance (runtime + VISUAL proof). Earlier "keys dead" was a test-methodology error

Built and ran the interactive acceptance test. Result: **keys work.** The whole "keys do
nothing / can't get out of the slideshow" premise was wrong — it was three test errors on my
side, not a firmware/emulator gap.

**KEY_NO_KEY = 0xa0** (`INPUT_KEY_GROUP9+0 = 160`). So the message the pump treats as "idle"
(§12.86.5's `0xa0`) is literally *no key*. The pump loop IS the key-processing loop: it reads
the current key (`0xa0` when none) and dispatches real keys to the active-UI handler `0x260f4`.

**The binary's real IR key map is at flash `0xe8b80`** (`_IRInfo.aIRMap`, size 0x60, loaded at
`0x425d8`; customer 0x00/0xFF, NEC). It does NOT match the `CONNTEL_IR2` source table in
`ir.h` — that mismatch is why my first injected scancodes decoded to KEY_NO_KEY. Real
scancode→key (the ones that matter):
`NEXT=0x10, PREV=0x11, ENTER=0x13, MENU=0x16, UP=0x17, RIGHT=0x4e, DOWN=0x52, LEFT=0x56,
STOP=0x5a, POWER=0x5d, EXIT=0x0c`.

**Runtime-confirmed key effects (full recipe, `CT952_IRKEY="<sc>@63000000"`):**
- **KEY_NEXT (0x10): advances the slideshow** — an extra JPEG decode fires right after
  injection (3 decodes vs the baseline's auto-advance), and a rendered frame at 68M differs
  from the no-key frame in **100% of pixels** (a completely different photo). Visual proof
  captured (`/tmp/frame_base.png` vs `/tmp/frame_next.png`).
- **KEY_STOP (0x5a): halts the auto-advance** — the ~71M auto-advance decode does NOT fire
  (2 decodes vs baseline 3). Pressing STOP stops the slideshow, exactly as expected.
- KEY_UP/MENU/ENTER: no decode change — nav/menu are gated during slideshow *playback*
  (you stop first, then browse), which is faithful photo-frame behavior.

**Why my earlier passes read "keys dead" (all my errors, corrected):**
1. Injected scancode 0x0c → decodes to **KEY_EXIT (0xc8)**, which is the **default no-op** in
   this UI (§12.88) — unrepresentative.
2. First "real" scancodes came from the `CONNTEL_IR2` **source** table, but the binary uses a
   **different `aIRMap`** (0xe8b80) — so they decoded to KEY_NO_KEY.
3. Watched only pump site `0xa720`; keys also flow through the pump's other sub-handlers
   (`0xa2c4/0xaf50/0xa414`), so `0xa720`-only counts missed them.

**Faithfulness verdict:** the retail boot reaches a **genuinely interactive photo slideshow**
that responds to the remote — NEXT/PREV navigate photos, STOP halts, and it auto-advances on
its own (internally posting KEY_NEXT). The boot-into-slideshow (not a power-on menu) is correct
for this build: POWERONMENU is only the no-configured-source fallback (§12.87), and this device
has configured sources + built-in demo media. No behavioural crutch is used for any of this
(`TICK_FAST_AT` only compresses the eCos wall-clock; `VDEC_IDLE`/`VSYNC_KEEP` are device-timing
models; injection is a probe). The screensaver/OSDSS idle path remains reachable per §12.81.

**Remaining (optional) polish:** to *see* on-screen nav feedback (cursor/thumbnail/menu OSD)
one would composite the browser's OSD overlay in `machine_disp_scanout`; the photo plane
already renders. And modeling a removable media source (card/USB) would let a source be
*opened* fresh — but the built-in demo already exercises the full interactive path.

### 12.90 ★ OSD overlay composites AND reacts to keys — nav keys visibly update the on-screen OSD (confirmed via `--fb-out`)

`machine_disp_scanout` (used by `--fb-out`) already composites the 8bpp OSD plane
(DS_OSDFRAME_ST 0x4005F000, palette = DISP GAM_OSD 0x80001C00) over the de-tiled photo
plane — index 0 transparent. Verified during the running slideshow:
- **OSD is enabled** (`0x1a54` programmed; scanout reports "OSD enabled") and draws a thin
  **top status/info bar** (rows ~2–12; ~0.3% of the panel). The rest is transparent so the
  photo shows through. Real OSD size reg `0x1a54 = 0x00f002d0` (720×240); the renderer clamps
  the OSD read to DS_OSDFRAME_END 0x40065000 (the video buffer start) to avoid the
  green-stripe artifact (§ earlier), which is fine for this bar.
- **The OSD REACTS to keys.** OSD-only renders (new `CT952_OSD_ONLY`: transparent→black so the
  drawn UI shows in isolation) diffed baseline vs injected key:
  - **KEY_UP (0x17): 971 OSD px change; KEY_DOWN (0x52): 951 px** — the top-bar element updates
    (an on-screen selection/info indicator moving), i.e. the OSD cursor/indicator reacts to
    navigation, exactly as expected.
  - KEY_PREV (0x11): 19 px (minor); KEY_STOP/MENU: no OSD change at that instant (they act on
    playback state, not this bar).
  Composite (`--fb-out`) frames differ identically (971/951 px), so the change is visible on
  the panel, not just the OSD plane.

### 12.91 ★ OSD STRIDE BUG FIXED — real stride is 480, not 720 (byte-level verified); the overlay was sheared

User flagged the OSD looked wrong ("stride error maybe?") — correct. The renderer defaulted to
OSD stride 720 (a guess), which sheared the drawn content: per-row analysis showed a 104px
block ping-ponging between x=440 and x=200 every row (a diagonal wrap), not a real glyph.

**Root, from a raw dump of the live OSD plane** (new `CT952_DRAMDUMP="<addr>:<len>@<icount>"`
→ /tmp/dramdump.bin; dumped 0x4005F000 len 0x6000): byte-level autocorrelation of the
non-zero mask peaks **sharply at stride 480** (score 728 at 480 vs 721 at 479/481, clean
falloff). The OSD uses palette indices 2 & 3 (text/box), content bytes at offset 5480..8943.
- Register cross-check: `REG_DISP_OSD_SIZE` (0x1a54)=0x00f002d0, `REG_DISP_SCREEN_SIZE`
  (0x1a44)=0x00f002d0 (`ctkav_disp.h`). The DRAM map (`dvd_dram_16m.h`) has TWO OSD frames:
  `DS_OSDFRAME` (DVD, 0x5F000..0x65000 = 24 KB, used in slideshow) and `DS_OSDFRAME_MM`
  (Media-Manager, 0x5F000..0xA2000 = 268 KB, the full browse UI). 24 KB / stride 480 = 51 rows
  (a top status strip), consistent with the content at rows 11..18.
- Rendered at stride 480 the content resolves to a **clean solid 104×8 box at (200,11)** — no
  shear (0 drift across rows). Fixed the `main.c` defaults: OSD `fb_w=480, fb_h=240,
  fb_stride=480` (was 616/440/720). `--fb-stride`/`--fb-wh` still override.

**New debug knobs:** `CT952_DRAMDUMP` (raw region dump), `CT952_OSD_ONLY` (render OSD plane on
black so drawn UI is visible in isolation). Remaining: in Media-Manager/browse mode the OSD is
the 268 KB `DS_OSDFRAME_MM` plane (full-screen thumbnails/menu) — its stride/geometry should be
re-derived the same way when that UI is entered (needs a browsable media source).

### 12.92 ★ OSD PALETTE COLOR BUG FIXED — GAM_OSD is RGB (0x00RRGGBB), not YUV; the box was magenta, now gray

User flagged the OSD colors were wrong (box magenta). Root: the scanout ran GAM_OSD entries
through a BT.601 `disp_yuv_to_rgb`, but on this firmware the OSD palette RAM stores **plain
0x00RRGGBB**. Confirmed from the live palette (`--iolog`, GAM_OSD window 0x80001C00+):
`[2]=0x00bbbbbb, [3]=0x01464646, [7]=0x00101010` — grays with **R==G==B**, which only holds
for RGB (a YUV gray would be U=V=0x80); `[4]=0x0f7d10 (green) [5]=0xcd1b24 (red)
[6]=0xe9ca2b (gold) [12]=0x0e499d (blue)` all read as sensible UI colours as RGB. The high
byte is an attribute/alpha flag (e.g. `0x01xxxxxx`) → mask to 24 bits. YUV-interpreting
`0xbbbbbb` gray produced the (255,128,255) magenta.

Fix: `pal[i] = raw & 0x00FFFFFF` (removed the unused `disp_yuv_to_rgb`). The slideshow OSD box
now renders 0xbbbbbb gray (matches the real UI's gray/blue scheme). The video/photo plane was
already correct (that path uses a separate, correct YUV→RGB in `video_sample_rgb`).

### 12.93 The gray/navy MEDIA_SELECT menu renders only its empty gray panel — the navy source-list text is never drawn (no media to list)

User: the UI should be "gray and navy blue," not gray with black text, and the text is cut off.
Root cause chain, verified from the live OSD plane + palette:
- We are in the **MEDIA_SELECT_DLG** (pump mode 8, §12.86-87) — a *menu*, which is why the
  expected scheme is gray panels + navy text.
- Full palette load traced (`CT952_PALTRACE`, GAM_OSD window, ACCESS_OSD set): a zero-init
  pass then a real pass loading **only indices 0..11** — `[2]=0xbbbbbb gray, [3]=0x464646
  dark-gray, [5]=0xcd1b24 red, [6]=0xe9ca2b gold, [11]=0x0e499d NAVY`; 12..255 = 0 (black).
  So navy IS in the palette, at index 11.
- But the OSD framebuffer (0x5F000..0x65000) uses **only indices 2 and 3** — `index 11 (navy)
  has ZERO pixels`. The lower full-width "ramp" (indices 27..78) seen when the clamp is lifted
  is the video buffer past 0x65000, not OSD.
- **Conclusion:** the menu draws its empty gray panel (idx 2 fill + idx 3 border) but never
  draws the navy source-list text, because there are **no media sources to list** (no card/USB
  modeled). The "cut off / black text" is the small empty panel, not real text.

So the color pipeline is now correct (stride 480 §12.91, RGB palette §12.92, navy present at
idx 11); the missing navy is *content*, not colour — it needs the browsable media source
modeled so the menu populates its list. New debug knob: `CT952_PALTRACE`.

**Net:** the retail boot renders a live photo slideshow with a composited OSD overlay whose
on-screen indicator updates in response to remote keys — the interactive path is faithful end
to end (input → firmware UI handler → OSD redraw → panel scan-out). The full-screen
thumbnail/menu grid (vs this status bar) would appear in the browse UI, which needs a
browsable media source; that (and any OSD-geometry widening past the 24 KB clamp for a
full-screen menu) is the only remaining polish. New debug knob: `CT952_OSD_ONLY`.

### 12.94 ★★ SD-CARD MODEL — the firmware inits a modeled SD card and enumerates its FAT filesystem; card presence changes the on-screen UI

Built a standard SD Host Controller model (SDHC spec, base 0xa0001100) backed by a FAT image
(`CT952_SDCARD=<path>`; image built by `mkfatimg.py`). Active only when a card image is loaded,
else the region stubs to 0 (card-less behaviour preserved). Confirmed the config from
`platform.h:27` (`CT909P_IC_SYSTEM`) → native SDC card reader; driver bodies are precompiled in
`card.a`/`sdc.o` (built from `/working/DMP_121_952/card/sdc.c`), register map = `ctkav_sdc.h`.

**Model:** `SDC_STAT` reports card-inserted/stable/CD-pin (0x24); `SW_RESET` (0x2f) self-clears;
`CLK_CTRL` (0x2c) reports internal-clock stable; responds to the full init chain
CMD0 / CMD8(R7) / ACMD41(OCR, CCS=1) / CMD2(CID R2) / CMD3(RCA R6) / CMD9(CSD v2 R2) / CMD7 /
ACMD6 / CMD6 / ACMD51(SCR) / CMD13; **CMD17/18 block reads DMA** straight into DRAM from the
image (`ARG`=block#, ×512), small reads (SCR/switch/status) via **PIO** DATA_PORT+BUFF_READ_RDY;
drives the INT_STAT CMD_COMPLETE / TRAN_COMPLETE handshake.

**Wake path (the make-or-break, confirmed):** with a card present, `CC_DVD_MainLoop`'s
`MEDIA_MonitorStatus` (every 200 ms, cc.c:1049) posts `USBSRC_CMD_CHECK_DEVICE`, waking
`USBSRC_Thread` (usbsrc.c:252, blocked on flag `_fUSBSRCCmdd`), which brings up the SD
controller and reads the card. So the media monitor DOES run — the card-less menu was empty
purely for lack of media, not because the monitor was dead.

**Verified end to end (runtime, `CT952_SDCTRACE`):** SW_RESET self-clear fix removed a 3.7 M-poll
hang; PIO DATA_PORT fix removed a 2.9 M INT_STAT-poll hang; then the firmware runs the whole
init chain and **CMD18-reads the FAT**: block 0 (BPB) → blocks 32-35 (root directory — all three
`01/02/03.JPG` 8.3 entries) → blocks 67-98 (01.JPG's data). i.e. it mounts the FS, enumerates
the root, finds the JPEGs, and reads the first one. **The OSD then changes from the empty gray
box to a full blue/white dialog** — proof the card-present path drives a different UI.

**Where it stops (next step):** the blue screen is the **COBY power-on splash** (user-identified;
GDI-drawn blue field + white COBY logo, no JPEG). With the card in, the firmware takes the
branded card-present boot branch: draw splash → enumerate card (BPB, root dir, 01.JPG header)
→ then **parks at the splash** — zero JPEG decodes in a 220 M-instr run, no further block reads,
and KEY_NEXT/ENTER are inert. So the storage+FS layer is faithful and complete (card mounts and
its files are found), but the firmware does not advance from the splash into decoding/displaying
the card's photos. The remaining work is that **splash → card-photo display transition**: find
what the splash waits on after enumeration (a slideshow/auto-play trigger, a "photos indexed"
event, or a further read the model isn't satisfying) and model it, then wire the JPU decode of a
card JPEG (from its DMA'd DRAM buffer, e.g. 0x401ec000) through the existing decode path. New
knobs: `CT952_SDCARD`, `CT952_SDCTRACE`; helper `mkfatimg.py`.

### 12.95 Card-photo stall pinned to the CC-event worker (the known keystone); SD DMA verified byte-correct

Pushed on the splash→photo transition, re-reading the verified facts first to avoid circling:
- The `0x40026e9c`/`0x40026ea4` flag-waits the media thread cycles on are **normal idle
  polling** (§12.75), NOT the stall — do not chase them.
- The hot `0x6c3xx` region is the demo slideshow's own JPU-kick chain (`0x6c500`, §12.79),
  not the card path.

New, verified this turn:
- **SD DMA is byte-correct.** Dumped `0x401ec000` after the block-67 read: it is 01.JPG's exact
  bytes (`FFD8FFE0…JFIF…Exif`), matching `950_Files/01.jpg` for the full 16 KB. So the storage
  path delivers valid JPEG data; the stall is not a data-corruption bug.
- **The card read runs in the CC-event worker.** Backtrace at the CMD18 issue PC `0xcb11c`
  (`SDC_ReadSector`): `0xcb11c ← 0xcb508 ← 0x16e2c ← 0x190b4 ← 0xd3dbc/0xd179c/0xcfdd0/0xd1310/
  0xcce48/0xd4d9c/0xd4e18` (card.a/info.a FS stack) `← 0x6308 ← 0x74cc` (the CC worker loop:
  `call 0x61b4; call 0x5969c` yield; dispatch). So parsing/loading the card is driven by the
  same CC-event worker that drives the demo slideshow.
- After reading 01.JPG's header the worker handler returns and yields; the next step (read the
  rest / decode / advance the MEDIA_Management state) needs a follow-on CC event that doesn't
  advance it — the firmware never points the JPU at the card buffer (`0x401ec000` never appears
  as `jpeg_src`, `CT952_JPEGTRACE`), so no card-photo decode is ever kicked.

**Honest verdict:** the SD-controller + FAT model is complete and byte-faithful (card mounts,
files enumerate, 01.JPG delivered correctly). The card-photo *display* transition sits at the
**CC-event-worker keystone** (§12.49-69) — the hardest long-standing item — reached now from the
card side. Cracking it means reversing the precompiled `info.a`/`card.a` parse/auto-play state
machine to find which CC event should advance it after the header read, and what should post
that event. That is a distinct, research-grade effort, not a near-term finish.

### 12.96 Card-photo stall re-pinned with fresh data: parse loads 01.JPG but never signals READY

Re-ran the card boot (`CT952_SDCARD=/tmp/sdcard.img CT952_TICK_MULT=64`) with a per-CMD18
sector trace, a media-state scan, and a JPU-source watch. Corrects several assumptions in
§12.94/95.

**Exact SD sequence (all CMD18, TICK_MULT=64):**
- 31.51M: full init chain CMD0/8/ACMD41/CMD2/CMD3/CMD9/CMD7/ACMD6/ACMD51/CMD6.
- 31.56M: READ sec 0 ×1 → 0x400293c8 (BPB).
- 31.58M: READ sec 0 ×4 → 0x401ff480 (BPB+reserved).
- 31.60M: READ sec 32 ×4 → 0x401fec60 (FAT2 tail + root-dir sector 35).
- 32.38M: READ sec 67 ×32 (**16 KB**) → **0x401ec000** (01.JPG file data, byte-correct).
- After 32.38M: **only the 200 ms card-detect STAT poll** (`0xa0001124`→0x00070000, PCs
  0x163fc/0x0c993c) forever. No more reads, no init retry.

**Verified facts (this turn):**
- The 16 KB read is a full file-data load (not a header peek). Backtrace at the load:
  `0xcb328 ← 0xcb51c ← 0x16e2c ← 0x190b4 ← info.a ← 0x6308 ← 0x74cc` — info.a's virtual-dispatch
  VFS (`ld [p+0xc]; call` indirections; retry-on-`-11` loop calling DRAM module 0x4001e138).
- After the load, info.a runs refcount cleanup (0xd5440: decrements a refcount, frees the buffer
  via DRAM mem-mgr 0x4001e918/0x4001e948) — consistent with the file parse *finishing*.
- **`jpeg_src` is ONLY EVER 0x401dc000 across the whole run** — the card buffer 0x401ec000 is
  never decoded. The 40 decodes are the demo/COBY-splash buffer.
- **UI stays mode-8 (MEDIA_SELECT_DLG)** the entire time; OSDSS screensaver is not active.
- **No MediaInfo state write after the load.** MediaInfo bStates are bitflags
  (INSERT=1/RECOGNIZE=2/PARSING=4/READY=8/WRONG=0x10). Scanning byte writes of those values in
  0x40020000–0x40040000 after 32.4M: **none** — no READY(8), no WRONG(0x10). So the parse result
  is never converted into a media-ready (or wrong-media) state that would drive auto-play.

**Refined keystone:** the SD + FAT + file-load path is complete and byte-faithful — the card's
first photo is correctly in DRAM at 0x401ec000. What never happens is the **info.a→media-manager
"parse done, media READY" handoff**: no MediaInfo→READY, no auto-play, no JPU decode of the card
buffer. It is NOT a data-corruption bug, NOT a WRONG-media rejection, and NOT the idle CC flags
(§12.75) or the demo JPU chain (§12.79). The parse reads everything correctly and then goes quiet
without raising READY.

**Delayed-insert test (disproves the edge-timing hypothesis).** Added `CT952_SDCARD_AT=<icount>`
to model the user inserting the card *after* power-on (STAT reports no-card until <icount>, giving
a clean insert edge). Ran with the card appearing at 40M. Result: **no card init at all** — zero
SD commands, decode stays 0x401dc000. The firmware polls card-detect only in a narrow boot window
(last STAT read 30.5M, returning 0=no-card), then **stops polling entirely**; when the card
appears at 40M nothing re-probes it. So media recognition here is effectively a **one-shot
boot-window probe** that requires the card present at ~30M. Present-from-boot is therefore the
faithful setup (real photo frames: card in, then power on) and the only path that reaches the
parse. Delaying the insert is strictly worse.

**Net:** present-from-boot → card inits + FAT enumerated + 01.JPG loaded to 0x401ec000 (byte-exact)
→ then parked with no READY. The remaining work is purely the info.a parse-complete→READY signal
(the flash `0xcxxxx/0xdxxxx` + DRAM `0x4001exxx` info.a module), which never raises the media-ready
state. New diagnostics landed for it: per-CMD18 sector trace + `CT952_SDCBT` load backtrace,
`CT952_MSCAN=<icount>` media-state-bitflag scan, `CT952_SDCARD_AT=<icount>` delayed insert.

### 12.97 The staged card file is ABANDONED — post-load processing never runs (read-watch proof)

Two decisive probes past §12.96:

- **Read-watch on the load buffer (`CT952_RDWATCH`).** After info.a DMA-loads 01.JPG's 16 KB into
  0x401ec000 (32.38M), **nothing ever reads it back** — 0 reads across the whole run, on *both*
  the 0x40 and 0xC0 (uncached) DRAM aliases. Combined with "`jpeg_src` never = 0x401ec000" (JPU
  never decodes it), the staged file is completely unused: no CPU header-parse, no decode. info.a
  enumerates the FAT, stages file #1, then **abandons it** — the post-load processing step (parse
  header → mark file valid → raise READY, or decode → display) never executes.

- **ENTER keypress does nothing (`CT952_IRKEY=0x13@33M`).** Delivered cleanly (IR ISR fires,
  IR_DATA=0x13, P1_2nd interrupt raised) but produces no UI change and no card decode — because
  the MEDIA_SELECT dialog (mode 8) has no READY source entry to select. Disproves the
  "waiting for user to pick the card" hypothesis; it loops back to the same missing READY.

**Keystone, sharpest form:** the parse loads the first photo correctly and then the processing
step that would inspect/decode it and raise MediaInfo→READY is never scheduled. It is not an SD
interrupt gap (the SD path is polling — firmware reads INT_STAT at 0xa0001130, no IRQ needed),
not edge-timing (§12.96), not user-select, not data. The trigger for the post-load step lives in
the precompiled info.a parse engine (flash 0xc/0xd + DRAM 0x4001e module) and is the next target.

### 12.98 ★★ CRACKED THE STALL — info.a kicks an unmodeled 0x80000800 JPEG engine and spins on its status

Traced the abandoned-buffer consumer (§12.97) with a value-watch (`CT952_BUFWATCH`, logs any
write carrying 0x401ec000). Right after the 16 KB load, the info.a parse hands the buffer to a
hardware engine at base **0x80000800**:
```
wr 80000a20 <- 401ec000   (source = the staged 01.JPG)   pc=0x9b968
wr 80000a28 <- 00000004   (param / unit count)
wr 80000a34 <- 80010200   (config / descriptor)
wr 80000a3c <- 00000001   (GO)
```
then polls **0x80000a30** bits[16:21] until they exceed 0x1f (spin at flash `0x9bcb4`). The
emulator did not model this block, so 0x80000a30 read 0 forever → infinite spin. Proof it is
card-specific: the `0x9bcb4` spin is the dominant loop *only* after a card parse; a no-card boot
never enters it (`CT952_PCSAMP` A/B).

**Model added** (machine.c): capture the source (0x80000a20) and GO (0x80000a3c); the status read
at 0x80000a30 reports full progress (bits[16:21]=0x3f) once kicked. Result: the `0x9bcb4` spin is
**gone** — the parse advances into new info.a code (DRAM module 0x4001d7xx: a find-first-set-bit
scan `0x4001d7e4` driven from deep recursive calls, i.e. real structure-walking work, not a hard
stall). So 0x80000800 is the JPEG parse/decode engine info.a uses to inspect each photo, and
modeling its completion is what lets the parse continue. Next: confirm the parse now reaches
MediaInfo→READY and decodes the card photo (long run in progress), and refine the engine so any
output the firmware reads back is correct.

### 12.99 Engine model advances the parse but does NOT display the photo — gated as opt-in

Honest status after §12.98. Modeling the 0x80000800 engine's completion removes the `0x9bcb4`
spin, but the on-screen result is UNCHANGED: the card boot still shows the COBY splash (the same
loading screen it showed before, §12.94) — no photo decode, no visual progress. What changed is
purely internal (the parse runs further). Do not read the splash as progress.

Findings on the engine consumer:
- The firmware reads **only** 0x80000a30 (status) back from the engine — once, at the spin
  (`0x9bca0`), value 0x003f0000 from the model. It never reads the raw JPEG at 0x401ec000
  (RDWATCH=0), nor the 0x80010000 region (the 0x80000a34<-0x80010200 target). So info.a relies
  entirely on the engine to process the photo and takes its result from... a channel not yet
  located (likely a DRAM output buffer set by the sibling register writes at 0x9b900, or extra
  bits of 0x80000a30 that the model zeroes).
- With the faked "done", info.a appears to REJECT the photo: MSCAN shows value 0x10 (MEDIA_WRONG)
  written to 0x40036e73/0x40036f53 (pc 0x6f1a0) right after the parse, and 02/03.JPG are never
  enumerated (no further SD reads). A spurious/empty engine result → wrong-media.
- Post-engine the CPU runs the eCos scheduler / CC-worker idle loop (window-flush at 0x4001d050
  driven from 0x70240→0x596a0), i.e. idle, not a hard hang.

Conclusion: the 0x80000800 JPEG engine is the verified root-cause of the long-standing card
stall (§12.49-69 keystone), but a *faithful* fix requires emulating what it actually produces
(decoded dimensions/validity) so info.a accepts the photo — the minimal "done" is not enough and
sends the firmware down the wrong-media path. Model is therefore **gated behind CT952_JPUENG** so
the default emulator stays honest (spins on the unmodeled engine). Next: locate the engine's real
output channel (disassemble the 0x9b800/0x9b900 setup that ran before the GO) and produce a valid
decode result, then re-check whether info.a accepts the card and auto-plays.

### 12.100 Full register map of the 0x80000800 parse engine (for the faithful-decode phase)

Complete programming captured (`CT952_ENGTRACE`, widened to 0x80000800-0x80000a80) at the card
parse (icount 32.384M, all from the setup fn at flash 0x9b8a4-0x9b9a4):
```
80000800 <- 00020000 then 00020001   control: mode bit17, then |1 = enable/start
80000894 <- 00000200 (=512)          width / stride
80000898 <- 00000240 (=576)          height?
80000820 <- 00080008                 block/MCU geometry (8x8)
8000080c <- 00000800 (=2048)
8000082c <- 00000000
80000844 <- 00040000 ; 80000848 <- 0010000c
80000a20 <- 401ec000                 SOURCE = staged 01.JPG
80000a28 <- 00000004
80000a34 <- 80010200                 DEST
80000a3c <- 00000001                 GO
80000a30  (read) bits[16:21]          STATUS/progress; firmware spins until >0x1f (0x9bcb4)
```
The same block is also written elsewhere (`80000894<-0x1000` at pc 0x851bc, seen at 10.6/18.2/
32.2M), so 0x80000800 is a shared 2D/JPEG/scaler engine, not card-only — but the card parse is
the only path that then *spins* on 0x80000a30 (no-card boot never enters 0x9bcb4, verified).

**Where it stands:** root cause of the card stall = this unmodeled engine (verified). Minimal
"done" model (CT952_JPUENG) clears the spin but the firmware then rejects the photo (needs the
real decoded result, not just a flag) and the screen is unchanged (COBY splash). The faithful
next phase is to emulate the engine: decode the JPEG at 0x80000a20 and deliver its result via the
0x80000a30 status bits and/or the 0x80010200 dest, matching what info.a reads, so the photo is
accepted and auto-played. That is a scoped but non-trivial hardware-block emulation task.

### 12.101 The 0x80000800 engine is shared with the display path — minimal model isn't faithful

Deepened §12.98-100. Key correction to earlier guesses (which were over-interpretations of single
byte-writes, now retracted): the value-8 at 0x40036ee0 is an OSD element descriptor, and the
value-0x10 at 0x40036e73 is an incidental struct-copy byte (pc 0x6f1a0 is a field-copy loop) —
NEITHER is a MediaInfo state, so there is no evidence the parse "rejected" the photo.

Solid new facts:
- The engine block layout matches the modeled GPU 2-D engine (SRC/DEST at offsets 0x94/0x98):
  0x80000800 is a second instance of the same 2D/JPEG/scaler engine (modeled one is at
  0xa0002880). It is **shared with the display path** — 0x80000894 is also written (0x1000) at
  pc 0x851bc during normal demo operation, independent of the card.
- info.a reads back ONLY the status (0x80000a30, once) from the engine — never the raw JPEG, the
  0x80010000 dest region, or any other engine register. So the parse's use of the engine needs
  only a correct completion/status, not a decoded-pixel readback.
- GO is issued ONCE for the card (src=0x401ec000, caller 0x8323c→0x9b9ac). With the minimal
  "done" model the 0x9bcb4 spin clears, but the flow then reaches a media/display state machine
  at 0x70240 (gated on 0x40039f60 / 0x40039f24) that does not advance, AND the demo slideshow
  stops — i.e. a card thread now spins in the eCos scheduler/window-flush (0x4001d050). So the
  minimal model trades one spin for a downstream one; a faithful model must reproduce the engine's
  real per-op semantics (busy→done transition per op, shared cleanly with the display path),
  not a latched permanent "done".

**Status:** root cause = the 0x80000800 2D/JPEG engine (verified, mapped). A faithful photo
display needs (a) proper per-op engine semantics, then (b) resolving the 0x70240 media/display
state machine that gates auto-play. Both scoped; neither done. Model stays gated (CT952_JPUENG).

### 12.102 Per-op engine semantics ruled out; the real downstream blocker is the 0x70240 display SM

Tested option-1 (per-op engine status: clear done on each new 0x80000a20 source write, set on GO)
instead of the permanent latch. Result: NO change — the demo still stops after the card parse
(only decode #1). So the engine-status model is not what stalls things; the card-parse thread
genuinely spins downstream and starves the demo thread.

The downstream blocker is the state machine at flash **0x70240** (reached only past the engine
spin). Its gate `0x70890` branches on:
- BRAM/NVRAM byte **0xb0000190** (persistent config; bit4/0x10 tested — and 0x70204 writes 0x10
  there), and
- OSD state bytes 0x40039f1c / 0x40039f24 (hword) / 0x40039f60 (state, cmp 0x21/0x80).

So the auto-play/display after parse is gated by a config-dependent OSD state machine, not the
engine. Since info.a reads back ONLY the engine status (not decoded output, §12.101), a faithful
engine "done" is sufficient for the *parse*; the remaining gap is this OSD/display state machine
advancing to actually decode+show the photo. That is the next target, and it is a genuine
reverse-engineering effort on config-gated display logic (0x70240 / 0x70890 / 0x71028), not a
one-line fix. Engine model stays gated (CT952_JPUENG).

### 12.103 Path-1 round: the display SM runs but no card-photo command is ever issued (blocker is upstream MM auto-play)

Watched the 0x70240 gate vars live (`CT952_SMWATCH`, JPUENG on):
- NVRAM `0xb0000190` **cycles** 0x11→0x80→0x21 repeatedly (writers 0x6f3f8/0x6f440), *identically*
  with and without a card (fires at 10.7M/18M in the demo, and 32M during the card parse). So the
  0x70240 display SM is alive and cycling — NOT frozen — but it is doing background refresh.
- OSD display-command `0x40039f60` stays **0** the whole time (only cleared at 0x70270, never set).
  0x70240 compares it to 0x21 and always takes the "nothing to display" exit. So no photo command
  is ever queued into it.
- The command-post fn 0x6f3c0 writes a command (%i1) + params (%i2) to a queue and mirrors
  0xb0000190, gated on the DSP state `0x40039948`==4 (the §12.67 7→0xd display-state var). This is
  the shared DSP display-event machinery, not card-specific.
- Confirmed the card never programs the **main JPU** either: `jpeg_src` never becomes 0x401ec000
  (the same path the demo uses for 0x401dc000). So the card photo gets no display kick by ANY route.

**Conclusion of this round:** the display subsystem is healthy and idle-cycling; the card photo is
never *submitted* for display. The blocker is upstream — the **MM auto-play** that (on media READY)
would point the JPU/display at 0x401ec000 never runs. So the next target is the media-READY →
MM-auto-play trigger: confirm whether the parse signals READY and what should launch auto-play.
Per-op engine model + SMWATCH diagnostics landed; model stays gated (CT952_JPUENG).

### 12.104 Post-engine: the item is finished and an event is signalled (0x4003d6a8) — handoff to a consumer thread

Followed the code right after the engine wait (0x9bcd0→0x9bd60→0x9be40): it indexes the current
item ([ctx+0x4d0]), checks per-item flag bits (0xc000/0x9000), reads a state byte at 0x4003d6a8+0x40,
sets a per-item done flag ([ctx+4+idx]=1), and **signals an eCos event/semaphore at 0x4003d6a8**
(`call 0x595d4`, i.e. flag-set/sem-post) to wake a consumer thread. So the parse side DOES finish
the photo item and post a completion event.

So the chain is now: info.a stages photo → 0x80000800 engine (modeled) → post-process + signal
0x4003d6a8 → [consumer thread] → (should) display. The photo never displays and the demo stops, so
the consumer side of 0x4003d6a8 either never runs the display or is gated out. This is the same
eCos event-handoff machinery as the CC-worker keystone (§12.49-75), now reached from the card side
one layer deeper.

**Honest position after the path-1 rounds:** the card-photo stall's ROOT CAUSE is cracked and
mapped (the 0x80000800 2D/JPEG engine, §12.98-100), and the post-parse handoff is now traced to a
specific event object (0x4003d6a8). Getting an actual photo on screen requires reversing the
consumer of that event through the DSP/MM display machinery (0x70240 SM, main-JPU kick) — a
sustained multi-layer effort across info.a + the eCos event system, not a near-term finish. All
diagnostics gated/landed; default emulator remains honest (spins on the unmodeled engine).

### 12.105 ★ CARD PHOTO RENDERS through the real pipeline (diagnostic probe) — remaining gap isolated to the auto-play trigger

Two clean facts nailed the decode model:
- **0x2a20 (JPU bit-stream source) is NEVER written** in either the card or the no-card boot. The
  emulator defaults jpeg_src=0x401dc000 and decodes whatever is staged there on a JPU kick. So the
  firmware displays a photo by DMA'ing it into the fixed decode buffer 0x401dc000, not by
  repointing the source.
- Buffer dump at 40M (card boot): 0x401dc000 holds the COBY splash (a valid 150-dpi JPEG),
  0x401ec000 holds 01.JPG (byte-identical to 950_Files/01.jpg). The card photo is never copied
  into the decode buffer -> the splash stays on screen.

**Diagnostic probe CT952_CARDSHOW=<icount>:** stage the FULL 01.JPG (card sector 67, 64KB — info.a
only loads the first 16KB header, which is why a decode of the parked buffer truncates) into
0x401ec000, point jpeg_src there, force one machine_maybe_jpeg_decode. Result: **JPEG decode #2:
720x405 from 0x401ec000** and the framebuffer shows the card photo (a pink rose) rendered correctly
through the real JPU + scanout path. Saved: jupiter/emu/card_photo_probe.png.

**What this proves (and doesn't):** the entire pipeline downstream of the auto-play trigger — SD +
FAT + full-file load + JPEG decode + tiled-YUV writeback + scanout — handles the card image
end-to-end. It is NOT the faithful boot: the probe force-loads the full file and forces the decode;
the firmware still does neither on its own. The remaining *faithful* gap is precisely the MM
auto-play trigger that would (1) load the full photo into the decode buffer and (2) kick the JPU —
gated on media READY, which needs the info.a incremental enumeration to finish all files (it
currently stops after item 1) and raise the parse-OK status. Probe gated (not default).

### 12.106 Last-mile: unfakeable proof set up; recognition never reports READY_MEDIA (honest wall)

Proof harness: rebuilt the card image with a DISTINCT test JPEG at sector 67 (bold color bands +
"SD-CARD / TEST IMAGE / not in demo album" text + cyan ellipse — nothing in the demo album). The
CT952_CARDSHOW probe renders exactly that image (decode #2: 640x360 from 0x401ec000) — so any
faithful render of it is unforgeable proof the SD path ran. Saved card_test_probe.png. Also
confirmed the demo slideshow is genuinely faithful (clean no-crutch boot renders a barn/Tetons
photo cycling, no STAGE_PHOTO/CARDSHOW) — the "faithful photo render" milestone is real; the card
photos are just the same album images, so the card path adds a code path, not a new picture (hence
the distinct test image).

Last-mile findings:
- The media-monitor thread stays alive after the parse (card-detect STAT polled every 200ms out to
  54M, sees card present) — it is NOT starved. But NO media-state byte transitions after the parse
  (no value-3/READY write in 0x40020000-0x40040000 past 31M). So the async USBSRC/CC-worker parse
  completes but never reports READY_MEDIA back to the manager -> no MM auto-play -> the display
  worker (0x830c8, polls 0x9b2cc for items) gets no "display photo" item -> splash stays.
- The recognition/READY gate hinges on the 0x80000800 engine's RESULT, but its output contract is
  opaque: dest 0x80010200 is unmapped (io array is only 0x8000), and info.a reads back ONLY the
  status reg 0x80000a30 (which the model already reports done). So faithfully satisfying recognition
  needs the engine's real decoded result (dimensions/valid), whose delivery channel is not
  determinable from the trace alone.

**Honest wall:** the faithful card auto-play is blocked on the 0x80000800 engine's output semantics
(a hardware JPEG header/thumbnail block) which can't be reversed from execution traces alone —
it needs hardware docs or the info.a/USBSRC source. Everything else is proven end-to-end (SD read,
FAT, full-file load, decode, scanout — via the distinct-image probe). Diagnostics all gated;
default emulator honest.

### 12.107 ★★ GHIDRA-VERIFIED full card-parse call chain — the gap is the parse-decision is never invoked

Using Ghidra (ghidra_re/), decompiled the entire card→display trigger chain to C. Definitive
(not hand-disassembly). The chain that SHOULD fire on card-present:

1. `FUN_0001b5c8` = PARSER-start (INFOFILTER_TriggerParsingInfo): sets `DAT_4003274a=1`
   (parser-enable), `DAT_40022f06=0`, and posts DSP **event 7** via `FUN_0005abb4(7)`.
2. `FUN_0005abb4` = DSP/buffer-mode SM (0x5abb4, the §12.68 keystone). In C: event 7 **remaps to
   mode 9 iff `DAT_4003274a!=0`**. Mode 9's case wires the card buffer `0x401ec000` + decode buffer
   `0x401dc000` and posts a DSP command `FUN_0005b028(0x42)`. `DAT_40039949` is the live mode.
3. `FUN_0001b5c8` is called ONLY from `FUN_0001b48c` (parse-decision): parses iff
   `DAT_40032780!=0 || FUN_000297f0()` (playable-file check, walks the file list `DAT_40032850`
   via `FUN_000255b4`); else does `OSD_ChangeUI(7)` (menu).
4. `FUN_0001b48c` is called from `FUN_0001f2e4` (media-recognize dispatch): calls the classifier
   `FUN_000180d4(src, cmd=DAT_4002fb2a)`; only when it returns 1 (bit 0x100 set — happens for
   cmd 0x32/0x41) does it reach `FUN_0001b48c`; otherwise `OSD_ChangeUI(7)`.

**Empirically (SMWATCH, card boot with JPUENG):**
- `DAT_4003274a` (parser-enable) is **only ever written 0, never 1** → `FUN_0001b5c8` never runs.
- Both branch markers of `FUN_0001b48c` (`DAT_40022f81` for menu, `DAT_4003274a` for parse) are
  never set → **`FUN_0001b48c` is never called at all**.
- `DAT_40039949` (DSP mode) reaches 7 once (10.8M) but never 9 (parse mode).

**So the gap, precisely:** the card-present media event never drives `FUN_0001f2e4` down the
"recognized playable, start parse" path (cmd 0x32/0x41 with classifier=1). Without that,
`FUN_0001b48c`→`FUN_0001b5c8` never fire, the DSP SM never enters mode 9, the parser never runs,
media never reaches READY, and the boot parks at the MEDIA_SELECT menu / splash. The next thread:
who calls `FUN_0001f2e4`, and what command/classifier result the card produces (`FUN_000180d4`,
`FUN_000255b4` file list) — all now decompilable in one place.
