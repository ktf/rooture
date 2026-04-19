cmake_minimum_required(VERSION 3.10...4.0)
project(rooture)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ROOT REQUIRED COMPONENTS Core MathCore RIO Hist Tree TreePlayer ROOTVecOps ROOTDataFrame ROOTNTuple)
include(${ROOT_USE_FILE})

add_executable(rooture rooture.cxx mpc.c)
target_include_directories(rooture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rooture PRIVATE
  edit
  ROOT::Core ROOT::MathCore ROOT::RIO
  ROOT::Hist ROOT::Tree ROOT::TreePlayer
  ROOT::ROOTVecOps ROOT::ROOTDataFrame ROOT::ROOTNTuple
)
target_compile_options(rooture PRIVATE -g -O0)
set_target_properties(rooture PROPERTIES
  INSTALL_RPATH "${ROOT_LIBRARY_DIR}"
  BUILD_WITH_INSTALL_RPATH TRUE
)

# Export symbols so Cling can resolve rooture bridge functions at runtime
if(APPLE)
  target_link_options(rooture PRIVATE -Wl,-export_dynamic)
else()
  target_link_options(rooture PRIVATE -rdynamic)
endif()

# Installation
install(TARGETS rooture RUNTIME DESTINATION bin)
install(FILES stdlib.rut DESTINATION share/rooture)
install(DIRECTORY examples/ DESTINATION share/rooture/examples)
