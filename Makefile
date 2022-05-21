DBG := #-fsanitize=address -ggdb3 -O0
SDL := $(shell pkg-config --cflags --libs sdl2)
ADL := src/adlmidi/src/*.cpp src/adlmidi/src/wopl/*.c src/adlmidi/src/chips/nuked_opl3.cpp src/adlmidi/src/chips/nuked_opl3_v174.cpp src/adlmidi/src/chips/nuked/*.c -DADLMIDI_DISABLE_DOSBOX_EMULATOR -DADLMIDI_DISABLE_OPAL_EMULATOR -DADLMIDI_DISABLE_JAVA_EMULATOR  -I src/adlmidi/include
all:
	g++ src/*.cpp src/sdl/*.cpp src/aux/*.cpp $(ADL) $(SDL) $(DBG) -I ./src/aux/ -o vikings
