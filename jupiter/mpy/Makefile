include micropython/py/mkenv.mk

# Cross-compile for big-endian SPARC V8 (CheerTek CT952/CT909), freestanding.
CROSS_COMPILE ?= sparc64-linux-gnu-

# qstr definitions (must come before including py.mk)
QSTR_DEFS = qstrdefsport.h

MICROPY_ROM_TEXT_COMPRESSION ?= 1

# include py core make definitions
include $(TOP)/py/py.mk

INC += -I.
INC += -Isysinc
INC += -I$(TOP)
INC += -I$(BUILD)

# SPARC V8, ILP32, big-endian, soft-float, no OS/libc. -I. makes our
# setjmp.h shim win over the toolchain's <setjmp.h>.
CFLAGS_ARCH = -m32 -mcpu=v8 -msoft-float -mno-app-regs
# -U_FORTIFY_SOURCE: the sparc64 headers otherwise inline snprintf() ->
# __snprintf_chk (a glibc fortify helper we don't link freestanding); readline
# uses snprintf, and shared/libc/printf.c supplies the plain version.
# -fno-delete-null-pointer-checks: ct952.peek32/poke32 legitimately dereference
# low/zero addresses (they're real MMIO/memory here), so the compiler must not
# treat a NULL deref as unreachable.
CFLAGS += $(INC) -Wall -std=c99 -nostdlib -ffreestanding -fno-common -U_FORTIFY_SOURCE -fno-delete-null-pointer-checks $(CFLAGS_ARCH) $(COPT)

CSUPEROPT = -Os

CFLAGS += -g
ifeq ($(DEBUG), 1)
CFLAGS += -O0
else
CFLAGS += -Os
CFLAGS += -fdata-sections -ffunction-sections
endif

# APP=1 builds the embedded "app" variant: a DRAM-resident payload (linked into
# the runtime-free 0x40500000 window, no trap table) that is injected into a
# booted firmware and launched in place of a firmware app. Use a separate BUILD
# dir so it doesn't clobber the standalone objects:
#   make APP=1 BUILD=build-app
ifdef APP
LDSCRIPT  = ct952_app.ld
# start_banner.S, not start_app.S: the latter's early asm paint reconfigures the
# display to an 8bpp plane at 0x4005F000, which CLOBBERS the AP loader's working
# 4bpp config on real silicon (proven on hardware -- see HW reference 10.17/10.20).
# start_banner.S is the same entry/trap table minus that paint.
STARTFILE = start_banner.S
# --build-id=none: drop the .note.gnu.build-id that would otherwise sit at the
# segment start and push .text off the window base. -N (OMAGIC): don't
# page-align. Together, .text starts exactly at 0x40500000, so the raw .bin
# loads 1:1 there and all absolute references resolve correctly.
LDFLAGS  += -Wl,-N -Wl,--build-id=none
else
LDSCRIPT  = ct952.ld
STARTFILE = start.S
endif

LD = $(CC)
LDFLAGS += -nostdlib -T $(LDSCRIPT) -Wl,-Map=$@.map,--cref -Wl,--gc-sections $(CFLAGS_ARCH)
LIBS =

SRC_C = \
	main.c \
	uart_core.c \
	modct952.c \
	modusb_kbd.c \
	shared/libc/string0.c \
	shared/libc/printf.c \
	shared/runtime/stdout_helpers.c \
	shared/runtime/pyexec.c \
	shared/readline/readline.c \

SRC_QSTR += modct952.c
SRC_QSTR += modusb_kbd.c
SRC_QSTR += shared/readline/readline.c

SRC_S = \
	$(STARTFILE) \

OBJ += $(PY_CORE_O)
OBJ += $(addprefix $(BUILD)/, $(SRC_C:.c=.o))
OBJ += $(addprefix $(BUILD)/, $(SRC_S:.S=.o))

all: $(BUILD)/firmware.bin

$(BUILD)/firmware.elf: $(OBJ)
	$(ECHO) "LINK $@"
	$(Q)$(LD) $(LDFLAGS) -o $@ $^ $(LIBS)
	$(Q)$(SIZE) $@

$(BUILD)/firmware.bin: $(BUILD)/firmware.elf
	$(ECHO) "BIN  $@"
	$(Q)$(OBJCOPY) -O binary -j .text -j .data $^ $@

include $(TOP)/py/mkrules.mk
