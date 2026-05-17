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

# STATIC=1: link libgcc + libstdc++ statically — binary runs on any glibc-compat
# Linux without matching toolchain versions. SDL2 stays dynamic (must be present
# on the target). Idempotent on WIN=1 (already includes the same flags).
STATIC ?= 0
ifeq ($(STATIC),1)
  PLATFORM_LDFLAGS += -static-libgcc -static-libstdc++
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
# HEADLESS: automated test build. Default mode (orig + v2 mirror), no SDL
# window, no audio device, no adlmidi link. Input from --replay-input, render
# to in-memory buffer (verified via A2), exit on first divergence with PPM dump.
# Designed for CI / fuzz testing. Mutually exclusive with V2_ONLY.
# Enable: `HEADLESS=1 make -j$(nproc)` → vikings_headless binary.
ifdef HEADLESS
V2_DEFINES += -DHEADLESS
EXE_NAME := vikings_headless
OBJDIR := .obj-headless
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
  src/sdl/v2_input_recorder.cpp \
  src/sdl/v2_keymap.cpp
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
  src/sdl/render.cpp \
  src/sdl/render_v2.cpp \
  src/sdl/render_v2_test.cpp \
  src/sdl/v2_render_funcs.cpp \
  src/sdl/v2_vm.cpp \
  src/sdl/v2_input_recorder.cpp \
  src/sdl/v2_keymap.cpp
# play.cpp: only in non-HEADLESS (HEADLESS uses headless_audio_stub.cpp instead)
ifndef HEADLESS
CXX_SRCS += src/sdl/play.cpp
endif
endif

# adlmidi sources — excluded in HEADLESS (no real audio playback, audit infra
# uses deterministic handles from v2_audit_compute_handle, no synthesis needed).
ifdef HEADLESS
ADL_SRCS :=
C_SRCS := \
  src/rendering/seg003_implementation.c \
  src/rendering/seg003_sdl_adapter.c
# HEADLESS extras: dump helpers + AudioPool stubs + headless main
CXX_SRCS += \
  src/sdl/headless/headless_main.cpp \
  src/sdl/headless/headless_dump.cpp \
  src/sdl/headless/headless_audio_stub.cpp
else
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
endif

CXX_SRCS += $(ADL_SRCS)
CXX_OBJS := $(patsubst %.cpp, $(OBJDIR)/%.o, $(CXX_SRCS))
C_OBJS   := $(patsubst %.c,   $(OBJDIR)/%.o, $(C_SRCS))
ALL_OBJS := $(CXX_OBJS) $(C_OBJS)
DEPS     := $(ALL_OBJS:.o=.d)

.PHONY: all clean keymap_editor

all: $(EXE_NAME)

# Standalone keymap editor — links ONLY against SDL2 + v2_keymap.cpp. No m2c,
# no adlmidi, no game logic. Independent of HEADLESS / V2_ONLY toggles.
KEYMAP_EDITOR_OBJDIR := .obj-keymap-editor
KEYMAP_EDITOR_SRCS := \
  src/sdl/v2_keymap.cpp \
  src/sdl/keymap_editor/editor.cpp
KEYMAP_EDITOR_OBJS := $(patsubst %.cpp, $(KEYMAP_EDITOR_OBJDIR)/%.o, $(KEYMAP_EDITOR_SRCS))

keymap_editor: vikings_keymap_editor

vikings_keymap_editor: $(KEYMAP_EDITOR_OBJS)
	$(CXX) $(DBG) $(PLATFORM_LDFLAGS) -o $@ $^ $(SDL)

$(KEYMAP_EDITOR_OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $(SDL) $(DBG) -MMD -MP -o $@ $<

-include $(KEYMAP_EDITOR_OBJS:.o=.d)

$(EXE_NAME): $(ALL_OBJS)
	$(CXX) $(DBG) $(PLATFORM_LDFLAGS) -o $@ $^ $(SDL)

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $(CXXFLAGS) $(ADL_DEFINES) -MMD -MP -o $@ $<

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) $(ADL_DEFINES) -MMD -MP -o $@ $<

clean:
	rm -rf .obj .obj-win .obj-headless .obj-keymap-editor vikings vikings.exe vikings_headless vikings_keymap_editor

-include $(DEPS)
