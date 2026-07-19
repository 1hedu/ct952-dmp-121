# ct952emu -- host build.
#
#   make            build ct952emu + test_sparc + the SPARC test image
#   make check      run the CPU verification
#   make boot       run the stock ROM (../..' DVD909.rom) briefly
#
# Cross toolchain for the test image: sparc64-linux-gnu-gcc -m32
# (big-endian SPARC V8, same as the firmware's ABI).

CC       ?= gcc
XCC      ?= sparc64-linux-gnu-gcc
XOBJCOPY ?= sparc64-linux-gnu-objcopy
CFLAGS   ?= -O2 -Wall

all: ct952emu test_sparc tests/testprog.bin

ct952emu: main.c sparc.c machine.c emujpeg.c picojpeg.c gdbstub.c sparc.h machine.h \
          emujpeg.h picojpeg.h
	$(CC) $(CFLAGS) -o $@ main.c sparc.c machine.c emujpeg.c picojpeg.c gdbstub.c

EMU_SRC = sparc.c machine.c emujpeg.c picojpeg.c gdbstub.c

test_sparc: test_sparc.c $(EMU_SRC) tests/testprog.c \
            sparc.h machine.h emujpeg.h picojpeg.h tests/testapi.h
	$(CC) $(CFLAGS) -I. -o $@ test_sparc.c $(EMU_SRC) \
	    tests/testprog.c

probe_unzip: probe_unzip.c $(EMU_SRC) sparc.h machine.h
	$(CC) $(CFLAGS) -I. -o $@ probe_unzip.c $(EMU_SRC)

# ---- SDK splash demo: real Jupiter code run on the emulated CT952 ----
XCFLAGS = -m32 -mcpu=v8 -msoft-float -O2 -ffreestanding -fno-builtin \
          -mno-app-regs -I.. -I../..

demo_run: demo_run.c $(EMU_SRC) sparc.h machine.h
	$(CC) $(CFLAGS) -I. -o $@ demo_run.c $(EMU_SRC)

tests/demo_splash.o: tests/demo_splash.c
	$(XCC) $(XCFLAGS) -c -o $@ $<

tests/jnes_s.o: ../jnes.c
	$(XCC) $(XCFLAGS) -c -o $@ $<

tests/jrgb2yuv_s.o: ../jrgb2yuv.c
	$(XCC) $(XCFLAGS) -c -o $@ $<

tests/demo_splash.elf: tests/start.o tests/demo_splash.o tests/jnes_s.o \
                       tests/jrgb2yuv_s.o tests/link.ld
	$(XCC) -m32 -nostdlib -static -Wl,-T,tests/link.ld -o $@ \
	    tests/start.o tests/demo_splash.o tests/jnes_s.o \
	    tests/jrgb2yuv_s.o -lgcc 2>/dev/null || \
	$(XCC) -m32 -nostdlib -static -Wl,-T,tests/link.ld -o $@ \
	    tests/start.o tests/demo_splash.o tests/jnes_s.o tests/jrgb2yuv_s.o

tests/demo_splash.bin: tests/demo_splash.elf
	$(XOBJCOPY) -O binary $< $@

demo: demo_run tests/demo_splash.bin
	./demo_run tests/demo_splash.bin demo_splash.ppm

tests/start.o: tests/start.S
	$(XCC) -m32 -mcpu=v8 -c -o $@ $<

tests/testprog.o: tests/testprog.c tests/testapi.h
	$(XCC) -m32 -mcpu=v8 -O2 -ffreestanding -fno-builtin \
	    -mno-app-regs -c -o $@ $<

tests/testprog.elf: tests/start.o tests/testprog.o tests/link.ld
	$(XCC) -m32 -nostdlib -static -Wl,-T,tests/link.ld \
	    -o $@ tests/start.o tests/testprog.o -lgcc 2>/dev/null || \
	$(XCC) -m32 -nostdlib -static -Wl,-T,tests/link.ld \
	    -o $@ tests/start.o tests/testprog.o

tests/testprog.bin: tests/testprog.elf
	$(XOBJCOPY) -O binary $< $@

tests/disp_test.o: tests/disp_test.c tests/testapi.h
	$(XCC) -m32 -mcpu=v8 -O2 -ffreestanding -fno-builtin \
	    -mno-app-regs -I. -c -o $@ $<

tests/disp_test.elf: tests/start.o tests/disp_test.o tests/link.ld
	$(XCC) -m32 -nostdlib -static -Wl,-T,tests/link.ld \
	    -o $@ tests/start.o tests/disp_test.o -lgcc 2>/dev/null || \
	$(XCC) -m32 -nostdlib -static -Wl,-T,tests/link.ld \
	    -o $@ tests/start.o tests/disp_test.o

tests/disp_test.bin: tests/disp_test.elf
	$(XOBJCOPY) -O binary $< $@

# Drive the DISP OSD plane via hardware registers and snapshot the
# emulator's modelled scan-out to a PPM.
disp: ct952emu tests/disp_test.bin
	./ct952emu tests/disp_test.bin --fb-out disp_test.ppm \
	    --instr 20000000 --quiet

tests/uart_test.o: tests/uart_test.c tests/testapi.h
	$(XCC) -m32 -mcpu=v8 -O2 -ffreestanding -fno-builtin \
	    -mno-app-regs -I. -c -o $@ $<

tests/uart_test.elf: tests/start.o tests/uart_test.o tests/link.ld
	$(XCC) -m32 -nostdlib -static -Wl,-T,tests/link.ld \
	    -o $@ tests/start.o tests/uart_test.o -lgcc 2>/dev/null || \
	$(XCC) -m32 -nostdlib -static -Wl,-T,tests/link.ld \
	    -o $@ tests/start.o tests/uart_test.o

tests/uart_test.bin: tests/uart_test.elf
	$(XOBJCOPY) -O binary $< $@

# Feed bytes into UART1 RX and verify the firmware echoes them back.
uart: ct952emu tests/uart_test.bin
	printf 'PING 1234\n' > uart_in.txt
	./ct952emu tests/uart_test.bin --uart-in uart_in.txt \
	    --uart uart_out.txt --instr 5000000 --quiet
	@printf 'sent: '; cat uart_in.txt
	@printf 'echo: '; cat uart_out.txt
	@cmp -s uart_in.txt uart_out.txt && echo "UART RX OK (echo matches)" \
	    || echo "UART RX MISMATCH"

check: test_sparc tests/testprog.bin
	./test_sparc tests/testprog.bin

boot: ct952emu
	./ct952emu ../../DVD909.rom --instr 50000000 --quiet \
	    --uart boot_uart.log --iolog boot_iolog.txt
	@echo "--- uart ---"; head -c 2000 boot_uart.log; echo
	@echo "--- iolog (head) ---"; head -30 boot_iolog.txt

# Boot the real DP700WD device image (in-tree) and snapshot the display.
# --skip-panelcfg carries boot past the config-arena frontier into display
# init (see BRINGUP.md); drop it to reproduce the raw stall.
bootreal: ct952emu
	./ct952emu dp700wd.bin --instr 2500000000 --quiet \
	    --uart dp700wd_boot.log --fb-out dp700wd_screen.ppm \
	    --fb-addr 0x4005f000 --fb-wh 480x234 --iolog dp700wd_iolog.txt
	@echo '--- boot log ---'; tr -c '[:print:]\n\t' ' ' < dp700wd_boot.log | tr -s ' '
	@echo; echo '--- boot screen written to dp700wd_screen.ppm (the OSD plane) ---'

# Functional JPEG decode: run the real firmware, let it stage the power-on
# COBY logo in DRAM, then decode that staged buffer with the emulator's own
# decoder -> a PPM. Proves the firmware->emulator decode path end to end.
logo: ct952emu
	CT952_PROC2=1 ./ct952emu dp700wd.bin --instr 800000000 --quiet \
	    --fb-addr 0x4005f000 --fb-wh 480x234 --decode-jpeg \
	    --jpeg-src 0x401dc000 --jpeg-out dp700wd_logo.ppm \
	    --fb-out dp700wd_logo_panel.ppm
	@echo "decoded firmware-staged logo -> dp700wd_logo.ppm"
	@echo "composited onto the 480x234 panel  -> dp700wd_logo_panel.ppm"

# Standalone decoder self-test: decode a JPEG file to a PPM.
jpeg_selftest: tests/jpeg_selftest.c emujpeg.c picojpeg.c emujpeg.h picojpeg.h
	$(CC) $(CFLAGS) -o $@ tests/jpeg_selftest.c emujpeg.c picojpeg.c

clean:
	rm -f ct952emu test_sparc jpeg_selftest tests/*.o tests/*.elf tests/*.bin \
	    boot_uart.log boot_iolog.txt
