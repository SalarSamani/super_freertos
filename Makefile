# ======================================================================
#  super_freertos/Makefile
# ======================================================================

CROSS      ?= riscv32-unknown-elf-
CC         := $(CROSS)gcc
OBJCOPY    := $(CROSS)objcopy
OBJDUMP    := $(CROSS)objdump

ARCHFLAGS  := -march=rv32imc -mabi=ilp32 -mcmodel=medany

FREERTOS   := kernel
PORT       := arch/riscv32

INCLUDES   := -I include \
              -I $(PORT) \
              -I mm \
              -I user \
              -I app

CFLAGS     := $(ARCHFLAGS) $(INCLUDES) -O2 -g -ffreestanding \
              -fno-builtin -Wall -Wextra -Werror \
              -fno-pic -fno-stack-protector \
              -DCONFIG_TIMER_RELOAD=500000
ASFLAGS    := $(ARCHFLAGS) $(INCLUDES) -g \
              -DCONFIG_TIMER_RELOAD=500000
LDFLAGS    := $(ARCHFLAGS) -nostdlib -Wl,-T,$(PORT)/kernel.ld \
              -Wl,--build-id=none -Wl,--gc-sections \
              -Wl,--no-check-sections

KERNEL_C   := $(FREERTOS)/tasks.c \
              $(FREERTOS)/queue.c \
              $(FREERTOS)/list.c \
              $(FREERTOS)/timers.c

PORT_C     := $(PORT)/port.c $(PORT)/mmu.c $(PORT)/string.c \
              $(PORT)/heap_placement.c $(PORT)/pmm.c \
              $(PORT)/slab.c $(PORT)/heap_slab.c $(PORT)/trap.c \
              $(PORT)/proc.c
PORT_S     := $(PORT)/start.S $(PORT)/portASM.S

MM_C       := mm/mm.c

USER_C     := user/user.c user/hello.c user/bad.c

APP_C      := app/main.c app/spawn.c

OBJS       := $(KERNEL_C:.c=.o) $(PORT_C:.c=.o) $(MM_C:.c=.o) \
              $(APP_C:.c=.o) $(USER_C:.c=.o) $(PORT_S:.S=.o)

ELF        := build/super_freertos.elf
BIN        := build/super_freertos.bin
VMEM       := build/super_freertos.vmem

all: $(VMEM)

$(ELF): $(OBJS) $(PORT)/kernel.ld | build
	$(CC) $(LDFLAGS) $(OBJS) -o $@
	$(OBJDUMP) -dSC $@ > $(@:.elf=.dis)
	$(OBJDUMP) -h  $@ > $(@:.elf=.map)

$(BIN): $(ELF)
	$(OBJCOPY) -O binary $< $@

# Ibex/Verilator convention: 32-bit words, little-endian, vmem format.
$(VMEM): $(BIN)
	srec_cat $< -binary -offset 0x0000 -fill 0x00 -within $< -binary -range-padding 4 -byte-swap 4 -o $@ -vmem

build:
	mkdir -p $@

clean:
	rm -f $(OBJS) $(FREERTOS)/heap_4.o build/* user/*.o

run: $(VMEM)
	$(IBEX_SIM)/Vibex_simple_system -c 0 --meminit=ram,$(ELF)

trace: $(VMEM)
	$(IBEX_SIM)/Vibex_simple_system -t -c 0 --meminit=ram,$(ELF)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
%.o: %.S
	$(CC) $(ASFLAGS) -c $< -o $@

.PHONY: all clean run trace