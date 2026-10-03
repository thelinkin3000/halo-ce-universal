# The memory probe, built the way LittleGPTracker's Switch port is built
# (projects/Makefile.SWITCH) rather than through switch_rules: the first
# hello probe used switch_rules and died before main() on hardware, while the
# same program built this way (probe/hello2) ran to completion.
export DEVKITPRO   := /opt/devkitpro
export DEVKITARM64 := $(DEVKITPRO)/devkitA64
export PORTLIBS    := $(DEVKITPRO)/portlibs/switch
export LIBNX       := $(DEVKITPRO)/libnx
export PATH        := $(DEVKITARM64)/bin:$(DEVKITPRO)/tools/bin:$(PATH)

PREFIX := aarch64-none-elf-
CC  := $(PREFIX)gcc
CXX := $(PREFIX)g++

ARCH_FLAGS := -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS     := -D__SWITCH__ -I$(LIBNX)/include -I$(PORTLIBS)/include $(ARCH_FLAGS) -O2 -Wall
ASFLAGS    := $(ARCH_FLAGS)
LDFLAGS    := -specs=$(LIBNX)/switch.specs -g $(ARCH_FLAGS)
LIBS       := -L$(LIBNX)/lib -lnx -L$(PORTLIBS)/lib -lpthread -lm

all: mem.nro

main.o: source/main.c
	$(CC) -MMD -MP -MF build/main.d $(CFLAGS) -c $< -o $@

# the payload the probe executes, assembled rather than hand-encoded: probe 4
# wrote the instruction's bytes by hand and got the immediate wrong
payload.o: source/payload.S
	$(CC) -c $(ASFLAGS) $< -o $@

mem.elf: main.o payload.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

mem.nacp:
	nacptool --create "Halo memory probe" "carlo" "0.1.0" $@

mem.nro: mem.elf mem.nacp
	elf2nro $< $@ --nacp=mem.nacp

clean:
	rm -rf build mem.elf mem.nacp mem.nro main.o payload.o