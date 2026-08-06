cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake")
endif()

project(TriflePsychotic C)

# vita_create_self / vita_create_vpk live here, not in the toolchain file
include("/usr/local/vitasdk/share/vita.cmake" REQUIRED)

set(CMAKE_C_STANDARD 99)
add_definitions(-DTRIFLE_DEBUG=0)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-q")

file(GLOB SOURCES "code/*.c")
list(REMOVE_ITEM SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/code/sdl_platform.c")
list(APPEND SOURCES "vita_platform.c")

include_directories(code)
include_directories("${VITASDK}/arm-vita-eabi/include/SDL2" "${VITASDK}/arm-vita-eabi/include")

add_executable(${PROJECT_NAME} ${SOURCES})

# order matters for the static linker: consumers before their deps
target_link_libraries(${PROJECT_NAME}
  SDL2_mixer
  SDL2_image
  SDL2

  # SDL2_mixer codec backends
  modplug
  opusfile
  opus
  xmp
  vorbisfile
  vorbis
  ogg

  # SDL2_image codec backends
  webpdemux
  webp
  jpeg
  png
  z

  m
  pthread
  stdc++

  SceAppMgr_stub
  SceAudio_stub
  SceAudioIn_stub
  SceCommonDialog_stub
  SceCtrl_stub
  SceDisplay_stub
  SceGxm_stub
  SceHid_stub
  SceIofilemgr_stub
  SceMotion_stub
  ScePower_stub
  SceProcessmgr_stub
  SceSysmodule_stub
  SceTouch_stub
)

vita_create_self(TriflePsychotic.self ${PROJECT_NAME} UNSAFE)

vita_create_vpk(TriflePsychotic.vpk TRFL00001 TriflePsychotic.self
  VERSION "01.00"
  NAME "Trifle Psychotic"
  FILE assets/gfx gfx
  FILE assets/data data
  FILE assets/audio audio
  FILE assets/mapmaking mapmaking
  FILE sce_sys sce_sys
)
