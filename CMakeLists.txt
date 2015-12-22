cmake_minimum_required(VERSION 2.8)
link_directories("~/work/active/sw3/osx_x86-64/ROOT/latest/lib")
include_directories("~/work/active/sw3/osx_x86-64/ROOT/latest/include")

add_executable(rooture rooture.cxx mpc.c)
target_link_libraries(rooture edit Core)
 
set(CMAKE_CXX_FLAGS -std=c++11)
# Installation
install(TARGETS rooture RUNTIME DESTINATION bin)
