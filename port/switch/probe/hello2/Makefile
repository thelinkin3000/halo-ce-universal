# A second hello probe, built the way LittleGPTracker's Switch port is built
# (projects/Makefile.SWITCH): the cross compiler named directly rather than
# through switch_rules, plain -march=armv8-a, and elf2nro given an explicit
# --nacp. The first hello probe was built with switch_rules and crashed on
# hardware; this one is the control.
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
LDFLAGS    := -specs=$(LIBNX)/switch.specs -g $(ARCH_FLAGS)
LIBS       := -L$(LIBNX)/lib -lnx -L$(PORTLIBS)/lib -lpthread -lm

all: hello2.nro

main.o: source/main.c
	$(CC) -MMD -MP -MF build/main.d $(CFLAGS) -c $< -o $@

hello2.elf: main.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

hello2.nacp:
	nacptool --create "Hello probe 2" "carlo" "0.1.0" $@

hello2.nro: hello2.elf hello2.nacp
	elf2nro $< $@ --nacp=hello2.nacp

clean:
	rm -rf build hello2.elf hello2.nacp hello2.nro main.o
