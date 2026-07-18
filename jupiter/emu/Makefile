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

ct952emu: main.c sparc.c machine.c sparc.h machine.h
	$(CC) $(CFLAGS) -o $@ main.c sparc.c machine.c

test_sparc: test_sparc.c sparc.c machine.c tests/testprog.c \
            sparc.h machine.h tests/testapi.h
	$(CC) $(CFLAGS) -I. -o $@ test_sparc.c sparc.c machine.c \
	    tests/testprog.c

probe_unzip: probe_unzip.c sparc.c machine.c sparc.h machine.h
	$(CC) $(CFLAGS) -I. -o $@ probe_unzip.c sparc.c machine.c

# ---- SDK splash demo: real Jupiter code run on the emulated CT952 ----
XCFLAGS = -m32 -mcpu=v8 -msoft-float -O2 -ffreestanding -fno-builtin \
          -mno-app-regs -I.. -I../..

demo_run: demo_run.c sparc.c machine.c sparc.h machine.h
	$(CC) $(CFLAGS) -I. -o $@ demo_run.c sparc.c machine.c

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

clean:
	rm -f ct952emu test_sparc tests/*.o tests/*.elf tests/*.bin \
	    boot_uart.log boot_iolog.txt

# ---- merged-in demos: multi-console gallery, SNES showcase, Mode 7 anim ----
tests/jgb_s.o: ../jgb.c
	$(XCC) $(XCFLAGS) -c -o $@ $<
tests/jgen_s.o: ../jgen.c
	$(XCC) $(XCFLAGS) -c -o $@ $<
tests/jsnes_s.o: ../jsnes.c
	$(XCC) $(XCFLAGS) -c -o $@ $<

tests/demo_gallery.o: tests/demo_gallery.c
	$(XCC) $(XCFLAGS) -c -o $@ $<
tests/demo_gallery.elf: tests/start.o tests/demo_gallery.o tests/jnes_s.o \
                        tests/jgb_s.o tests/jgen_s.o tests/jrgb2yuv_s.o tests/link.ld
	$(XCC) -m32 -nostdlib -static -Wl,-T,tests/link.ld -o $@ \
	    tests/start.o tests/demo_gallery.o tests/jnes_s.o tests/jgb_s.o \
	    tests/jgen_s.o tests/jrgb2yuv_s.o -lgcc 2>/dev/null || \
	$(XCC) -m32 -nostdlib -static -Wl,-T,tests/link.ld -o $@ \
	    tests/start.o tests/demo_gallery.o tests/jnes_s.o tests/jgb_s.o \
	    tests/jgen_s.o tests/jrgb2yuv_s.o
tests/demo_gallery.bin: tests/demo_gallery.elf
	$(XOBJCOPY) -O binary $< $@
gallery: demo_run tests/demo_gallery.bin
	./demo_run tests/demo_gallery.bin demo_gallery.ppm

tests/demo_snes.o: tests/demo_snes.c
	$(XCC) $(XCFLAGS) -c -o $@ $<
tests/demo_snes.elf: tests/start.o tests/demo_snes.o tests/jsnes_s.o tests/link.ld
	$(XCC) -m32 -nostdlib -static -Wl,-T,tests/link.ld -o $@ \
	    tests/start.o tests/demo_snes.o tests/jsnes_s.o -lgcc 2>/dev/null || \
	$(XCC) -m32 -nostdlib -static -Wl,-T,tests/link.ld -o $@ \
	    tests/start.o tests/demo_snes.o tests/jsnes_s.o
tests/demo_snes.bin: tests/demo_snes.elf
	$(XOBJCOPY) -O binary $< $@
snes: demo_run tests/demo_snes.bin
	./demo_run tests/demo_snes.bin demo_snes.ppm

anim_run: anim_run.c sparc.c machine.c sparc.h machine.h
	$(CC) $(CFLAGS) -I. -o $@ anim_run.c sparc.c machine.c
tests/demo_anim.o: tests/demo_anim.c
	$(XCC) $(XCFLAGS) -c -o $@ $<
tests/demo_anim.elf: tests/start.o tests/demo_anim.o tests/jsnes_s.o tests/link.ld
	$(XCC) -m32 -nostdlib -static -Wl,-T,tests/link.ld -o $@ \
	    tests/start.o tests/demo_anim.o tests/jsnes_s.o -lgcc 2>/dev/null || \
	$(XCC) -m32 -nostdlib -static -Wl,-T,tests/link.ld -o $@ \
	    tests/start.o tests/demo_anim.o tests/jsnes_s.o
tests/demo_anim.bin: tests/demo_anim.elf
	$(XOBJCOPY) -O binary $< $@
anim: anim_run tests/demo_anim.bin
	./anim_run tests/demo_anim.bin anim.bin
	python3 tools/mkgif.py anim.bin demo_mode7.gif
