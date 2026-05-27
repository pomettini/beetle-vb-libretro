SDK = $(shell egrep '^\s*SDKRoot' ~/.Playdate/config | head -n 1 | cut -c9-)

HEAP_SIZE  = 12582912
STACK_SIZE = 61800
PRODUCT    = VirtualBoy.pdx
OBJDIR     = build
DEPDIR     = $(OBJDIR)/dep

# ── Detect host OS ──────────────────────────────────────────────────────────
detected_OS := $(shell uname -s)

ifeq ($(detected_OS), Darwin)
  SIMCOMPILER = clang
  DYLIB_FLAGS = -dynamiclib -rdynamic
  DYLIB_EXT   = dylib
endif
ifeq ($(detected_OS), Linux)
  SIMCOMPILER = gcc
  DYLIB_FLAGS = -shared -fPIC
  DYLIB_EXT   = so
endif

# ── ARM cross-toolchain ─────────────────────────────────────────────────────
TRGT   = arm-none-eabi-
GCC   := $(dir $(shell which $(TRGT)gcc 2>/dev/null))
ifeq ($(GCC),)
  GCC = /usr/local/bin/
endif

CC    = $(GCC)$(TRGT)gcc
CXX   = $(GCC)$(TRGT)g++
CP    = $(GCC)$(TRGT)objcopy
PDC   = $(SDK)/bin/pdc

MCU   = cortex-m7
FPU   = -mfloat-abi=hard -mfpu=fpv5-sp-d16 -D__FPU_USED=1
OPT   = -O2 -falign-functions=16 -fomit-frame-pointer

# ── Preprocessor defines ────────────────────────────────────────────────────
DEFINES  = -DTARGET_PLAYDATE=1 -DTARGET_EXTENSION=1
DEFINES += -DWANT_8BPP=1
DEFINES += -DNDEBUG
DEFINES += -DVB_DISABLE_AUDIO=1
DEFINES += -DMEDNAFEN_VERSION=\"0.9.31\" -DMEDNAFEN_VERSION_NUMERIC=931
DEFINES += -D__HEAP_SIZE=$(HEAP_SIZE) -D__STACK_SIZE=$(STACK_SIZE)
# VB_SCANLINES: 0=off (full quality), 1=black-gap scanlines (retro CRT look), 2=duplicate lines (same density)
DEFINES += -DVB_SCANLINES=0
# Strip Run_Accurate (5744 dead bytes) since we always init in FAST mode
DEFINES += -DVB_V810_FAST_ONLY=1

# ── Include paths ────────────────────────────────────────────────────────────
INCDIR  = -I .
INCDIR += -I src
INCDIR += -I mednafen
INCDIR += -I mednafen/include
INCDIR += -I libretro-common/include
INCDIR += -I $(SDK)/C_API

# ── Compile flags ────────────────────────────────────────────────────────────
MCFLAGS  = -mthumb -mcpu=$(MCU) $(FPU)

CFLAGS   = $(MCFLAGS) $(OPT) -g3 -gdwarf-2
CFLAGS  += -Wall -Wno-unused -Wstrict-prototypes -Wno-unknown-pragmas
CFLAGS  += -fverbose-asm -Wdouble-promotion -mword-relocations -fno-common
CFLAGS  += -ffunction-sections -fdata-sections
CFLAGS  += $(DEFINES) $(INCDIR)

CXXFLAGS  = $(MCFLAGS) $(OPT) -g3 -gdwarf-2
CXXFLAGS += -Wall -Wno-unused -Wno-unknown-pragmas
CXXFLAGS += -fverbose-asm -Wdouble-promotion -mword-relocations -fno-common
CXXFLAGS += -ffunction-sections -fdata-sections
CXXFLAGS += -std=c++14 -fno-exceptions -fno-rtti
CXXFLAGS += $(DEFINES) $(INCDIR)

# ── Linker flags ─────────────────────────────────────────────────────────────
LDSCRIPT = link_map.ld
LDFLAGS  = -nostartfiles $(MCFLAGS)
LDFLAGS += -T$(LDSCRIPT)
LDFLAGS += -Wl,-Map=$(OBJDIR)/pdex.map,--cref,--gc-sections,--no-warn-mismatch,--emit-relocs

# ── Source files ─────────────────────────────────────────────────────────────
C_SRC  = src/main.c
C_SRC += src/vb_display.c
C_SRC += src/vb_input.c
C_SRC += src/vb_audio.c
C_SRC += src/mempatcher_stub.c
C_SRC += src/syscalls_stub.c
C_SRC += mednafen/vb/vip.c
C_SRC += mednafen/vb/vsu.c
C_SRC += mednafen/vb/timer.c
C_SRC += mednafen/vb/input.c
C_SRC += mednafen/sound/Blip_Buffer.c
C_SRC += mednafen/settings.c
C_SRC += mednafen/state.c
C_SRC += mednafen/hw_cpu/v810/fpu-new/softfloat.c
C_SRC += libretro-common/compat/compat_strl.c
C_SRC += $(SDK)/C_API/buildsupport/setup.c

CPP_SRC  = src/vb_core.cpp
CPP_SRC += src/cxx_support.cpp
CPP_SRC += mednafen/hw_cpu/v810/v810_cpu.cpp

ALL_SRC = $(C_SRC) $(CPP_SRC)

# ── Object files ─────────────────────────────────────────────────────────────
C_OBJS   = $(addprefix $(OBJDIR)/,$(C_SRC:.c=.o))
CPP_OBJS = $(addprefix $(OBJDIR)/,$(CPP_SRC:.cpp=.o))
OBJS     = $(C_OBJS) $(CPP_OBJS)

# ── Simulator flags ───────────────────────────────────────────────────────────
SIM_DEFINES  = -DTARGET_SIMULATOR=1 -DTARGET_EXTENSION=1
SIM_DEFINES += -DWANT_32BPP=1
SIM_DEFINES += -DMEDNAFEN_VERSION=\"0.9.31\" -DMEDNAFEN_VERSION_NUMERIC=931
SIM_DEFINES += -D__HEAP_SIZE=$(HEAP_SIZE) -D__STACK_SIZE=$(STACK_SIZE)

SIM_INCDIR  = -I . -I src -I mednafen -I mednafen/include
SIM_INCDIR += -I libretro-common/include -I $(SDK)/C_API

SIM_CFLAGS   = -g $(SIM_DEFINES) $(SIM_INCDIR) -Wall -Wno-unused -Wstrict-prototypes
SIM_CXXFLAGS = -g $(SIM_DEFINES) $(SIM_INCDIR) -Wall -Wno-unused -std=c++14 -fno-exceptions -fno-rtti

# Simulator uses host clang/clang++ for separate compilation
SIM_CC  = clang
SIM_CXX = clang++

SIM_C_OBJS   = $(addprefix $(OBJDIR)/sim/,$(C_SRC:.c=.o))
SIM_CPP_OBJS = $(addprefix $(OBJDIR)/sim/,$(CPP_SRC:.cpp=.o))

# ── Targets ───────────────────────────────────────────────────────────────────
.PHONY: all device simulator clean

all: device simulator
	$(PDC) Source $(PRODUCT)

device: Source/pdex.elf

simulator: Source/pdex.$(DYLIB_EXT)

Source/pdex.elf: $(OBJDIR)/pdex.elf | Source
	cp $< $@

Source/pdex.$(DYLIB_EXT): $(OBJDIR)/pdex.$(DYLIB_EXT) | Source
	cp $< $@

$(OBJDIR)/pdex.elf: $(OBJS)
	$(CXX) $(OBJS) $(LDFLAGS) -o $@

$(OBJDIR)/pdex.$(DYLIB_EXT): $(SIM_C_OBJS) $(SIM_CPP_OBJS)
	$(SIM_CXX) $(DYLIB_FLAGS) -lm -o $@ $^

$(OBJDIR)/sim/%.o: %.c | $(OBJDIR)
	@mkdir -p $(dir $@)
	$(SIM_CC) -c $(SIM_CFLAGS) $< -o $@

$(OBJDIR)/sim/%.o: %.cpp | $(OBJDIR)
	@mkdir -p $(dir $@)
	$(SIM_CXX) -c $(SIM_CXXFLAGS) $< -o $@

# ── Compile rules ─────────────────────────────────────────────────────────────

# vb_core: -mlong-calls so that ITCM functions (copied to a new address) use
# LDR+BX via literal pool for all outgoing calls instead of PC-relative b.w/bl.
$(OBJDIR)/src/vb_core.o: src/vb_core.cpp | $(OBJDIR)
	@mkdir -p $(dir $@)
	$(CXX) -c $(CXXFLAGS) -mlong-calls $< -o $@

# v810 interpreter: -Os to minimize I-cache footprint (16KB Rev B); align loops to cache lines
$(OBJDIR)/mednafen/hw_cpu/v810/v810_cpu.o: mednafen/hw_cpu/v810/v810_cpu.cpp | $(OBJDIR)
	@mkdir -p $(dir $@)
	$(CXX) -c $(filter-out -O2 -falign-functions=16,$(CXXFLAGS)) -Os -falign-functions=32 -falign-loops=32 $< -o $@

$(OBJDIR)/%.o: %.c | $(OBJDIR)
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) $< -o $@

$(OBJDIR)/%.o: %.cpp | $(OBJDIR)
	@mkdir -p $(dir $@)
	$(CXX) -c $(CXXFLAGS) $< -o $@

$(OBJDIR):
	mkdir -p $(OBJDIR)

Source:
	mkdir -p Source

clean:
	-rm -rf $(OBJDIR)
	-rm -rf $(PRODUCT)
	-rm -f Source/pdex.elf
	-rm -f Source/pdex.dylib
	-rm -f Source/pdex.so
