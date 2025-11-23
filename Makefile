# Build optimization level.
# Default: -O0 for debug stepping (variables visible, exact call stacks).
# RELEASE=1: -O2 for performance (inlining, vectorization, loop unrolling).
# Keep -ggdb3 in both — debug symbols stay for crash backtraces / gdb / perf.
# -fno-omit-frame-pointer in RELEASE: keep frame pointer for accurate
#   stack traces (perf record --call-graph dwarf still works without, but
#   fp-based unwinding is faster + works without DWARF in tight inner loops).
RELEASE ?= 0
ifeq ($(RELEASE),1)
DBG    := -ggdb3 -O2 -fno-omit-frame-pointer
else
DBG    := -ggdb3 -O0 #-fsanitize=address
endif

# WIN=1: cross-compile to Windows x86_64 via mingw-w64.
# Prereqs (Arch / Arch ARM via AUR — yay/paru):
#   mingw-w64-gcc, mingw-w64-sdl2, mingw-w64-pkg-config
# Build:  make clean && make WIN=1 -j$(nproc)  → vikings.exe (statically linked).
# Linux-only debug code (execinfo backtrace, perf_event_open HW watchpoints,
# dlfcn/dladdr, sys/mman ring buffer) is compiled out via #ifdef __linux__
# and replaced with no-op stubs — gameplay unaffected, only platform-native
# instrumentation is lost.
WIN ?= 0
ifeq ($(WIN),1)
MINGW_TRIPLET ?= x86_64-w64-mingw32
CC         := $(MINGW_TRIPLET)-gcc
CXX        := $(MINGW_TRIPLET)-g++
PKG_CONFIG ?= $(MINGW_TRIPLET)-pkg-config
SDL        := $(shell $(PKG_CONFIG) --cflags --libs sdl2)
# -D_WIN32_WINNT=0x0601: required by mingw 13+ thread library (mcfgthread) for
# std::thread / std::mutex — Win7 ABI is the minimum.
# -static-libgcc/-libstdc++: bundle C++ runtime so the .exe needs only SDL2.dll
# at runtime, not a full mingw redist. (Full -static would need libSDL2.a which
# Nix's pkgsCross.mingwW64.SDL2 does not provide — only the import lib.)
# Drop Linux-only -rdynamic / -no-pie.
PLATFORM_DEFINES := -D_WIN32_WINNT=0x0601
PLATFORM_LDFLAGS := -static-libgcc -static-libstdc++
EXE_NAME   := vikings.exe
OBJDIR     := .obj-win
else
CC         := gcc
CXX        := g++
SDL        := $(shell pkg-config --cflags --libs sdl2)
PLATFORM_DEFINES :=
PLATFORM_LDFLAGS := -rdynamic -no-pie
EXE_NAME   := vikings
OBJDIR     := .obj
endif

ADL_DEFINES := -DADLMIDI_DISABLE_DOSBOX_EMULATOR \
               -DADLMIDI_DISABLE_OPAL_EMULATOR \
               -DADLMIDI_DISABLE_JAVA_EMULATOR

INCLUDES := -I ./src/aux/ -I ./src/rendering/ -I src/adlmidi/include

V2_DEFINES := -DV2_RENDER_FROM_SHADOW
# V2_ONLY: build v2 standalone (skip orig m2c main loop + DS-hash compare).
# When defined, orig sub_* functions are not called per-frame; v2 phase functions
# handle everything on shadow DS. v2 sub_12352 reads SDL input directly.
# Enable via env: `V2_ONLY=1 make -j$(nproc)` — disabled by default.
ifdef V2_ONLY
V2_DEFINES += -DV2_ONLY
endif

CXXFLAGS := $(SDL) $(DBG) $(INCLUDES) $(V2_DEFINES) $(PLATFORM_DEFINES)
CFLAGS   := $(SDL) $(DBG) $(INCLUDES) $(V2_DEFINES) $(PLATFORM_DEFINES)

ifdef V2_ONLY
# V2_ONLY: m2c-decompiled files NOT compiled. v2_main.cpp is the entry point.
# Excluded: vikings.exe*.cpp, _data.cpp, asm.cpp, shadowstack.cpp, memmgr.cpp (all m2c-only).
# Static EXE data (seg001 text/menu, seg004 tables, etc.) is loaded at runtime
# from `exe_static.bin` — a snapshot of m2c::m[0..0x29F00] taken once in default
# mode after the C++ Initializer in vikings.exe.cpp populates it (= the static
# image baked into the original DOS .EXE binary).
CXX_SRCS := \
  src/sdl/v2_main.cpp \
  src/sdl/play.cpp \
  src/sdl/render.cpp \
  src/sdl/render_v2.cpp \
  src/sdl/render_v2_test.cpp \
  src/sdl/v2_render_funcs.cpp \
  src/sdl/v2_vm.cpp \
  src/sdl/v2_input_recorder.cpp
else
CXX_SRCS := \
  src/vikings.exe.cpp \
  src/vikings.exe_default_seg.cpp \
  src/vikings.exe_seg000.cpp \
  src/vikings.exe_seg002.cpp \
  src/vikings.exe_seg003.cpp \
  src/_data.cpp \
  src/aux/asm.cpp \
  src/aux/memmgr.cpp \
  src/aux/shadowstack.cpp \
  src/sdl/play.cpp \
  src/sdl/render.cpp \
  src/sdl/render_v2.cpp \
  src/sdl/render_v2_test.cpp \
  src/sdl/v2_render_funcs.cpp \
  src/sdl/v2_vm.cpp \
  src/sdl/v2_input_recorder.cpp
endif

# Common adlmidi sources (always compiled)
ADL_SRCS := \
  src/adlmidi/src/adlmidi.cpp \
  src/adlmidi/src/adlmidi_load.cpp \
  src/adlmidi/src/adlmidi_midiplay.cpp \
  src/adlmidi/src/adlmidi_opl3.cpp \
  src/adlmidi/src/adlmidi_private.cpp \
  src/adlmidi/src/adlmidi_sequencer.cpp \
  src/adlmidi/src/inst_db.cpp \
  src/adlmidi/src/chips/nuked_opl3.cpp \
  src/adlmidi/src/chips/nuked_opl3_v174.cpp

C_SRCS := \
  src/rendering/seg003_implementation.c \
  src/rendering/seg003_sdl_adapter.c \
  src/adlmidi/src/wopl/wopl_file.c \
  src/adlmidi/src/chips/nuked/nukedopl3_174.c \
  src/adlmidi/src/chips/nuked/nukedopl3.c

CXX_SRCS += $(ADL_SRCS)
CXX_OBJS := $(patsubst %.cpp, $(OBJDIR)/%.o, $(CXX_SRCS))
C_OBJS   := $(patsubst %.c,   $(OBJDIR)/%.o, $(C_SRCS))
ALL_OBJS := $(CXX_OBJS) $(C_OBJS)
DEPS     := $(ALL_OBJS:.o=.d)

.PHONY: all clean

all: $(EXE_NAME)

$(EXE_NAME): $(ALL_OBJS)
	$(CXX) $(DBG) $(PLATFORM_LDFLAGS) -o $@ $^ $(SDL)

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $(CXXFLAGS) $(ADL_DEFINES) -MMD -MP -o $@ $<

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) $(ADL_DEFINES) -MMD -MP -o $@ $<

clean:
	rm -rf .obj .obj-win vikings vikings.exe

-include $(DEPS)
