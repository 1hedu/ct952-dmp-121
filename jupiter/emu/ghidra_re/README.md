# Ghidra headless RE of dp700wd.bin

Decompiler-assisted RE of the retail firmware (SPARC V8 BE, raw binary, XIP @ vaddr==flash off).

## Setup (reproduce)
    # Ghidra 11.3.2 (needs JDK 17+; JDK 21 present)
    curl -sSL -o /tmp/ghidra.zip \
      https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_11.3.2_build/ghidra_11.3.2_PUBLIC_20250415.zip
    cd /tmp && unzip -q ghidra.zip
    # import + analyze (once, ~3 min):
    /tmp/ghidra_11.3.2_PUBLIC/support/analyzeHeadless /tmp/ghproj ct952 \
      -import dp700wd.bin -processor sparc:BE:32:default \
      -loader BinaryLoader -loader-baseAddr 0x0

## Decompile functions to C
    export CT952_DECOMP="0x1b800,0x6f054,0x375a0"   # comma-sep addrs (any addr in the fn)
    /tmp/ghidra_11.3.2_PUBLIC/support/analyzeHeadless /tmp/ghproj ct952 \
      -process dp700wd.bin -noanalysis \
      -scriptPath <this dir> -postScript Decompile.java
    # strip "INFO  Decompile.java> " prefix from output lines.

## Key map (from strings + decompiled C)
- Debug strings recovered (`strings dp700wd.bin`): "THUMB: trigger -> PARSERHEADER OK/upsupport/fail/timeout",
  "_JPEGFMT_ReadImageHeader/QuantizationTable/HuffmanTable", "CalcMcScaleRatio: MC_H>8x",
  "media status is _bSourceMenuMediaStatus[0x%x]=0x%x", "INFOFILTER_RecognizeMedia: BOOK_*",
  "MEDIA_Management: Umount File System", "Parser stop fail", "delete parser thread".
- FUN_0001b800  = PARSERHEADER driver (thumbnail parse). State DAT_4003263c: 0=START -> 1=header-wait
  -> 2=decode-wait. Gated on DAT_4003274a. Prints the PARSERHEADER strings (0xe7348 etc.).
- FUN_000375a0  = decode-status mapper. Calls FUN_0006f054(x,0) -> reads _DAT_40039cd0 (decode state).
  For param2 in {0,1}: state 0x10 -> 1(OK); 0x11 -> 0(fail); 0x20/0 -> 2(retry); 0x12 -> 3(unsupported).
- FUN_0006f054  = HAL decode-status getter (action select). case0: status = _DAT_40039cd0 (|0x1000 unless
  DAT_b0000190 in {0,0x11}). cases 1..0xe read other _DAT_40039cxx fields.
- FUN_00083098 (0x830c8) = display/decode WORKER: polls FUN_0009b2cc for items (_DAT_4003d6d4 count),
  processes each (type 0 -> kicks the 0x80000800 thumbnail engine at 0x9b9ac), yields.

## Open question (last mile)
Card boot: the 0x80000800 engine IS kicked for the card photo (src=0x401ec000, from the worker),
but PARSERHEADER (DAT_4003263c) never advances past 0 -> the parse-record path never runs -> media
never reaches READY -> no auto-play. Need: who posts the worker item, and what should trigger
PARSERHEADER / set _bSourceMenuMediaStatus to a ready value.
