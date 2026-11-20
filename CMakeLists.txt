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
  src/core.cpp
  src/state.cpp
  src/daemon.cpp
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

add_executable(integration_tests tests/integration.cpp)
target_link_libraries(integration_tests PRIVATE fmtrans_core)
target_compile_definitions(integration_tests PRIVATE FMTRANSD_PATH="$<TARGET_FILE:fmtransd>")
add_test(NAME integration COMMAND integration_tests)
add_test(NAME cli COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_test.sh
         $<TARGET_FILE:fmtrans>)
set_tests_properties(cli PROPERTIES TIMEOUT 180)
