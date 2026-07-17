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

tests/demo_gallery.o: tests/demo_gallery.c
	$(XCC) $(XCFLAGS) -c -o $@ $<

tests/jgb_s.o: ../jgb.c
	$(XCC) $(XCFLAGS) -c -o $@ $<

tests/jgen_s.o: ../jgen.c
	$(XCC) $(XCFLAGS) -c -o $@ $<

tests/demo_gallery.elf: tests/start.o tests/demo_gallery.o tests/jnes_s.o \
                        tests/jgb_s.o tests/jgen_s.o tests/jrgb2yuv_s.o \
                        tests/link.ld
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

check: test_sparc tests/testprog.bin
	./test_sparc tests/testprog.bin

boot: ct952emu
	./ct952emu ../../DVD909.rom --instr 50000000 --quiet \
	    --uart boot_uart.log --iolog boot_iolog.txt
	@echo "--- uart ---"; head -c 2000 boot_uart.log; echo
	@echo "--- iolog (head) ---"; head -30 boot_iolog.txt

clean:
	rm -f ct952emu test_sparc tests/*.o tests/*.elf tests/*.bin \
	    boot_uart.log boot_iolog.txt
