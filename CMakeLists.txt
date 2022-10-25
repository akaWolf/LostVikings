cmake_minimum_required(VERSION 3.17)

project(LostVikings
    VERSION 1.0
    DESCRIPTION "Lost Vikings"
    LANGUAGES C CXX)

file(GLOB
	ADL_FILES
	${PROJECT_SOURCE_DIR}/src/adlmidi/src/*.cpp
	${PROJECT_SOURCE_DIR}/src/adlmidi/src/wopl/wopl_file.c
	${PROJECT_SOURCE_DIR}/src/adlmidi/src/chips/nuked_opl3.cpp
	${PROJECT_SOURCE_DIR}/src/adlmidi/src/chips/nuked_opl3_v174.cpp
	${PROJECT_SOURCE_DIR}/src/adlmidi/src/chips/nuked/*.c)
add_compile_definitions(ADLMIDI_DISABLE_DOSBOX_EMULATOR
	ADLMIDI_DISABLE_OPAL_EMULATOR
	ADLMIDI_DISABLE_JAVA_EMULATOR)
include_directories(${PROJECT_SOURCE_DIR}/src/adlmidi/include)


file(GLOB
	VIKINGS_FILES
	${PROJECT_SOURCE_DIR}/src/*.cpp
	${PROJECT_SOURCE_DIR}/src/sdl/*.cpp
	${PROJECT_SOURCE_DIR}/src/aux/*.cpp)
include_directories(${PROJECT_SOURCE_DIR}/src/aux)

find_package(SDL2 REQUIRED)
include_directories(${SDL2_INCLUDE_DIRS})

add_executable(vikings ${VIKINGS_FILES} ${ADL_FILES})
target_link_libraries(vikings ${SDL2_LIBRARIES})
