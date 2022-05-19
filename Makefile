SDL := $(shell pkg-config --cflags --libs sdl2)
all:
	g++ src/*.cpp src/aux/*.cpp $(SDL) -ggdb3 -O0 -I ./src/aux/ -o vikings
