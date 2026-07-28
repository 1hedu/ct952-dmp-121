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
CFLAGS += $(INC) -Wall -std=c99 -nostdlib -ffreestanding -fno-common -U_FORTIFY_SOURCE $(CFLAGS_ARCH) $(COPT)

CSUPEROPT = -Os

CFLAGS += -g
ifeq ($(DEBUG), 1)
CFLAGS += -O0
else
CFLAGS += -Os
CFLAGS += -fdata-sections -ffunction-sections
endif

LD = $(CC)
LDFLAGS += -nostdlib -T ct952.ld -Wl,-Map=$@.map,--cref -Wl,--gc-sections $(CFLAGS_ARCH)
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
	start.S \

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
