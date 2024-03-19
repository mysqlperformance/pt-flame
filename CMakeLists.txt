cmake_minimum_required(VERSION 2.8.12)
project(pt_flame C CXX)
find_package(Threads REQUIRED)

set(SOURCES src/driver.cpp src/perfetto.cpp src/reader.cpp src/replay.cpp)
set(SCRIPTS
  script/pt_pstack.sh script/pt_drawflame.sh script/pt_drawflame_compat.sh
  script/flamegraph.pl)
add_executable(pt_flame ${SOURCES})

add_library(pt_filter SHARED src/script_filter.cpp)
set_target_properties(pt_filter PROPERTIES PREFIX "")

if(CMAKE_VERSION VERSION_LESS "3.8.0")
  target_compile_options(pt_flame PRIVATE "-std=c++17")
else()
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED True)
endif()

target_link_libraries(pt_flame ${CMAKE_THREAD_LIBS_INIT})
install(TARGETS pt_flame DESTINATION bin)
install(PROGRAMS ${SCRIPTS} DESTINATION bin)
install(TARGETS pt_filter DESTINATION lib)
