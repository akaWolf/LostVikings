DBG    := -ggdb3 -O0 #-fsanitize=address
SDL    := $(shell pkg-config --cflags --libs sdl2)
OBJDIR := .obj

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

CXXFLAGS := $(SDL) $(DBG) $(INCLUDES) $(V2_DEFINES)
CFLAGS   := $(SDL) $(DBG) $(INCLUDES) $(V2_DEFINES)

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
  src/sdl/v2_vm.cpp
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
  src/sdl/v2_vm.cpp
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

all: vikings

vikings: $(ALL_OBJS)
	g++ $(DBG) -rdynamic -no-pie -o $@ $^ $(SDL)

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	g++ -c $(CXXFLAGS) $(ADL_DEFINES) -MMD -MP -o $@ $<

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	gcc -c $(CFLAGS) $(ADL_DEFINES) -MMD -MP -o $@ $<

clean:
	rm -rf $(OBJDIR) vikings

-include $(DEPS)
