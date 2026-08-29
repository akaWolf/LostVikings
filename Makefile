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
# Linux without matching toolchain versions. Idempotent on WIN=1 (already
# includes the same flags).
STATIC ?= 0
ifeq ($(STATIC),1)
  PLATFORM_LDFLAGS += -static-libgcc -static-libstdc++
endif

# SDL_STATIC=1: link SDL2 statically via `pkg-config --static` — binary no
# longer needs libSDL2 at runtime. Requires libSDL2.a (Ubuntu's libsdl2-dev
# and SDL2 mingw-devel ship it; some distros split it into a separate -static
# package). On Linux, system audio/video runtime libs (X11, ALSA, dbus, ...)
# are still loaded dynamically by SDL2 itself.
SDL_STATIC ?= 0
ifeq ($(SDL_STATIC),1)
  # PKG_CONFIG is set in the WIN=1 block; default for native Linux build.
  PKG_CONFIG ?= pkg-config
  SDL := $(shell $(PKG_CONFIG) --cflags --libs --static sdl2)
  # `pkg-config --static` lists transitive deps as -l, but gcc still picks
  # libSDL2.so over libSDL2.a (and on mingw, libSDL2.dll.a over libSDL2.a).
  # Replace -lSDL2 with -l:libSDL2.a to force the static archive specifically
  # — system libs (X11, ALSA, mingw32, ...) keep their default lookup.
  SDL := $(patsubst -lSDL2,-l:libSDL2.a,$(SDL))
  ifeq ($(WIN),1)
    # SDL2main on Windows expects the user to define SDL_main(); we have a
    # plain main() instead (in asm.cpp / v2_main.cpp / headless_main.cpp).
    # Strip from pkg-config output:
    #   -lSDL2main      — SDL2main library (would also call SDL_main)
    #   -mwindows       — GUI subsystem flag (mingw startup would expect WinMain)
    #   -Dmain=SDL_main — mingw sdl2.pc injects this in Cflags, which renames
    #                     our main() to SDL_main BEFORE the source sees
    #                     SDL_MAIN_HANDLED. Must be filtered too.
    # Then add -DSDL_MAIN_HANDLED (so SDL.h does not rename main inside source)
    # and -mconsole (so mingw startup expects plain main()).
    SDL := $(filter-out -lSDL2main -mwindows -Dmain=SDL_main,$(SDL))
    PLATFORM_DEFINES += -DSDL_MAIN_HANDLED
    PLATFORM_LDFLAGS += -mconsole
  endif
endif

ADL_DEFINES := -DADLMIDI_DISABLE_DOSBOX_EMULATOR \
               -DADLMIDI_DISABLE_OPAL_EMULATOR \
               -DADLMIDI_DISABLE_JAVA_EMULATOR

INCLUDES := -I ./src/aux/ -I ./src/rendering/

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

# V2_ONLY objects live in their own objdir so switching default<->V2_ONLY
# never mixes configurations (multiple-definition trap) and never forces a
# clean rebuild. Suffix applied AFTER the HEADLESS assignment so the combo
# builds compose (.obj-v2only / .obj-headless-v2only); -nogen/-bounds
# suffixes below compose on top as before. The binary name stays `vikings`.
ifdef V2_ONLY
OBJDIR := $(OBJDIR)-v2only
endif

# Stage-3A transpiled executors (src/sdl/gen/*.gen.inc) are the DEFAULT in
# every build: the VM fetch-decode loop survives only as the unknown-pc
# fallback (its removal is scheduled for the end of stage B, together with
# the opcode table, once the inlining passes retire the handlers).
# Runtime A/B switch: V2_GENCODE=0 env. Compile-time opt-out: NOGENCODE=1
# (separate objdir/binary suffix so the two configurations never mix).
ifdef NOGENCODE
EXE_NAME := $(EXE_NAME)_nogen
OBJDIR := $(OBJDIR)-nogen
else
V2_DEFINES += -DV2_GENCODE
endif

# V2_GS_BOUNDS=1: stage-4 II.b bounds sanitizer — view accessors report
# (dedup) every runtime-indexed access that leaves its field span. Separate
# objdir/binary so the instrumented build never mixes with the canon one.
ifdef V2_GS_BOUNDS
V2_DEFINES += -DV2_GS_BOUNDS
EXE_NAME := $(EXE_NAME)_bounds
OBJDIR := $(OBJDIR)-bounds
endif

# COV_SEG000=1: instrument ONLY the m2c oracle (vikings.exe_seg000.cpp) with
# gcov, for the fn-test coverage report (task #47). Everything else compiles
# as usual; the link adds --coverage for the gcov runtime. Use with HEADLESS:
#   COV_SEG000=1 HEADLESS=1 make -j$(nproc)
#   FNSELFTEST=all ./vikings_headless > /tmp/fnst_cov.log 2>&1
#   gcov --json-format -o .obj-headless/src .obj-headless/src/vikings.exe_seg000.o
#   python3 python/fn_coverage_report.py vikings.exe_seg000.cpp.gcov.json.gz
ifdef COV_SEG000
COV_LDFLAGS := --coverage
V2_DEFINES += -DFT_COV_BUILD
endif

CXXFLAGS := $(SDL) $(DBG) $(INCLUDES) $(V2_DEFINES) $(PLATFORM_DEFINES)
CFLAGS   := $(SDL) $(DBG) $(INCLUDES) $(V2_DEFINES) $(PLATFORM_DEFINES)

# (direction IV) v2_hash_hot.cpp is a pure-function -O2 island: the replay-
# verify hash kernels were ~77% of CPU at the project-wide -O0.
# gcc takes the LAST -O flag, so appending wins over the -O0 in $(DBG).
$(OBJDIR)/src/sdl/v2_hash_hot.o: CXXFLAGS += -O2
# v2_vm.cpp peaks >13G under -ggdb3 var-tracking in the V2_ONLY+HEADLESS
# combo (gen includes + the asset facade) — cap the debug detail for this
# one unit; everything else keeps full -ggdb3.
$(OBJDIR)/src/sdl/v2_vm.o: CXXFLAGS += -g1 -fno-var-tracking-assignments

# (#85) The m2c world is NOT -O2-clean: at -O2 the translated goto-labyrinth
# miscompiles (attract: a stray stack word inside seg002 sub_1c155 shifts the
# far-ret frame — "Return address wasn't created by native CALL"; inserting
# opaque no-op probes made it vanish, the classic UB-sensitive-codegen smell).
# RELEASE=1 therefore pins every m2c translation unit at -O0 and lets the
# whole v2/sdl/aux layer take -O2. The emulated world was ~6% of CPU in the
# attract profile — the -O0 pin costs little.
ifeq ($(RELEASE),1)
$(OBJDIR)/src/vikings.exe.o: CXXFLAGS += -O0
$(OBJDIR)/src/vikings.exe_default_seg.o: CXXFLAGS += -O0
$(OBJDIR)/src/vikings.exe_seg000.o: CXXFLAGS += -O0
$(OBJDIR)/src/vikings.exe_seg002.o: CXXFLAGS += -O0
$(OBJDIR)/src/vikings.exe_seg003.o: CXXFLAGS += -O0
$(OBJDIR)/src/_data.o: CXXFLAGS += -O0
endif

ifdef V2_ONLY
# V2_ONLY: m2c-decompiled files NOT compiled. v2_main.cpp is the entry point.
# Excluded: vikings.exe*.cpp, _data.cpp, asm.cpp, shadowstack.cpp, memmgr.cpp (all m2c-only).
# Static EXE data (seg001 text/menu, seg004 tables, etc.) is loaded at runtime
# from `exe_static.bin` — a snapshot of m2c::m[0..0x29F00] taken once in default
# mode after the C++ Initializer in vikings.exe.cpp populates it (= the static
# image baked into the original DOS .EXE binary).
CXX_SRCS := \
  src/sdl/v2_main.cpp \
  src/sdl/render.cpp \
  src/sdl/render_v2.cpp \
  src/sdl/render_v2_test.cpp \
  src/sdl/v2_render_funcs.cpp \
  src/sdl/v2_vm.cpp \
  src/sdl/v2_hash_hot.cpp \
  src/sdl/v2_input_recorder.cpp \
  src/sdl/v2_native_opl.cpp \
  src/sdl/v2_ail_interp.cpp \
  src/sdl/v2_ail.cpp \
  src/sdl/v2_ail_native.cpp \
  src/sdl/v2_gamestate.cpp \
  src/sdl/v2_assets.cpp \
  src/sdl/v2_keymap.cpp
# play.cpp: only in non-HEADLESS (HEADLESS uses headless_audio_stub.cpp instead)
ifndef HEADLESS
CXX_SRCS += src/sdl/play.cpp
endif
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
  src/sdl/v2_hash_hot.cpp \
  src/sdl/v2_input_recorder.cpp \
  src/sdl/v2_keymap.cpp \
  src/sdl/v2_fn_test.cpp \
  src/sdl/v2_native_opl.cpp \
  src/sdl/v2_ail_interp.cpp \
  src/sdl/v2_ail.cpp \
  src/sdl/v2_ail_native.cpp \
  src/sdl/v2_gamestate.cpp \
  src/sdl/v2_assets.cpp
# play.cpp: only in non-HEADLESS (HEADLESS uses headless_audio_stub.cpp instead)
ifndef HEADLESS
CXX_SRCS += src/sdl/play.cpp
endif
endif

ifdef HEADLESS
C_SRCS := \
  src/rendering/seg003_implementation.c \
  src/rendering/seg003_sdl_adapter.c
# HEADLESS extras: dump helpers + AudioPool stubs + headless main
CXX_SRCS += \
  src/sdl/headless/headless_main.cpp \
  src/sdl/headless/headless_dump.cpp \
  src/sdl/headless/headless_audio_stub.cpp
else
# #79: the adlmidi player library is GONE — only the Nuked OPL3 core stays
# (the render chip of the native interpreted AIL driver, v2_native_opl.cpp).
C_SRCS := \
  src/rendering/seg003_implementation.c \
  src/rendering/seg003_sdl_adapter.c \
  src/adlmidi/src/chips/nuked/nukedopl3.c
endif
CXX_OBJS := $(patsubst %.cpp, $(OBJDIR)/%.o, $(CXX_SRCS))
# (#61) native AIL channel: the Nuked OPL3 core is already in the default
# C_SRCS; HEADLESS builds (audio stub) need it explicitly for the silent
# render + V2_OPL_TRACE verification channel.
ifdef HEADLESS
C_SRCS += src/adlmidi/src/chips/nuked/nukedopl3.c
endif
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

# Editor binary name picks up .exe under WIN=1 so the same target works for
# both native Linux and mingw cross-compile.
KEYMAP_EDITOR_EXE := vikings_keymap_editor$(if $(filter 1,$(WIN)),.exe,)

keymap_editor: $(KEYMAP_EDITOR_EXE)

$(KEYMAP_EDITOR_EXE): $(KEYMAP_EDITOR_OBJS)
	$(CXX) $(DBG) $(PLATFORM_LDFLAGS) -o $@ $^ $(SDL)

$(KEYMAP_EDITOR_OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $(SDL) $(DBG) $(PLATFORM_DEFINES) -MMD -MP -o $@ $<

-include $(KEYMAP_EDITOR_OBJS:.o=.d)

$(EXE_NAME): $(ALL_OBJS)
	$(CXX) $(DBG) $(PLATFORM_LDFLAGS) $(COV_LDFLAGS) -o $@ $^ $(SDL)

ifdef COV_SEG000
$(OBJDIR)/src/vikings.exe_seg000.o: CXXFLAGS += --coverage
endif

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $(CXXFLAGS) $(ADL_DEFINES) -MMD -MP -o $@ $<

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) $(ADL_DEFINES) -MMD -MP -o $@ $<

clean:
	rm -rf .obj .obj-* vikings vikings.exe vikings_headless vikings_keymap_editor vikings_keymap_editor.exe

-include $(DEPS)
