cmake_minimum_required(VERSION 2.8)

set(ROOT_LIB_PATH ${ROOT_ROOT}/lib)
set(ROOT_INCLUDE_PATH ${ROOT_ROOT}/include)
link_directories(${ROOT_LIB_PATH})
include_directories(${ROOT_INCLUDE_PATH})

add_custom_command(OUTPUT DictOutput.cxx DictOutput.h
  COMMAND rootcint
  ARGS -f DictOutput.cxx -c ROOTureApp.h LinkDef.h
  DEPENDS ROOTureApp.h LinkDef.h
)

add_executable(rooture rooture.cxx DictOutput.cxx mpc.c)
target_link_libraries(rooture edit Core Cint MathCore RIO)
 
set(CMAKE_CXX_FLAGS "-g -O0 -std=c++11")
# Installation
install(TARGETS rooture RUNTIME DESTINATION bin)
