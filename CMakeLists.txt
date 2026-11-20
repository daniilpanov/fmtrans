cmake_minimum_required(VERSION 3.16)
project(fmtrans LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

find_package(Threads REQUIRED)

add_library(fmtrans_core STATIC
  src/sha1.cpp
  src/net.cpp
)
target_include_directories(fmtrans_core PUBLIC include third_party)
target_link_libraries(fmtrans_core PUBLIC Threads::Threads)

add_executable(fmtransd src/main_daemon.cpp)
target_link_libraries(fmtransd PRIVATE fmtrans_core)

add_executable(fmtrans src/main_cli.cpp)
target_link_libraries(fmtrans PRIVATE fmtrans_core)

enable_testing()

add_executable(unit_tests tests/unit.cpp)
target_link_libraries(unit_tests PRIVATE fmtrans_core)
add_test(NAME unit COMMAND unit_tests)
