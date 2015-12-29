cmake_minimum_required(VERSION 2.8)
link_directories("~/work/active/sw/osx_x86-64/ROOT/latest/lib")
include_directories("~/work/active/sw/osx_x86-64/ROOT/latest/include")

add_custom_command(OUTPUT DictOutput.cxx DictOutput.h
  COMMAND rootcint
  ARGS -f DictOutput.cxx -c ROOTureApp.h LinkDef.h
  DEPENDS ROOTureApp.h LinkDef.h
)

add_executable(rooture rooture.cxx DictOutput.cxx mpc.c)
target_link_libraries(rooture edit Core Cint)
 
set(CMAKE_CXX_FLAGS "-g -O0 -std=c++11")
# Installation
install(TARGETS rooture RUNTIME DESTINATION bin)
