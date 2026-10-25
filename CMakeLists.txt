# Blast Arena - Bomberman-style multiplayer game (Cyclone DDS + OpenGL/GLFW).
# Build:  cmake -B build && cmake --build build -j && ctest --test-dir build

cmake_minimum_required(VERSION 3.16)
project(BlastArena VERSION 1.0 LANGUAGES C)

# ---------------------------------------------------------------------------
# Build settings
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)   # use -DCMAKE_BUILD_TYPE=Debug for gdb
endif()
add_compile_options(-Wall -Wextra)

# Optional builds:
#   cmake -B build -DBA_PROFILE=ON   -> gprof
#   cmake -B build -DBA_SANITIZE=ON  -> AddressSanitizer + UBSan
option(BA_PROFILE  "Build with gprof instrumentation (-pg)" OFF)
option(BA_SANITIZE "Build with Address/UB sanitizers"       OFF)
if(BA_PROFILE)
  add_compile_options(-pg)
  add_link_options(-pg)
endif()
if(BA_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Dependencies
#   sudo apt install cyclonedds-dev cyclonedds-tools libglfw3-dev libgl1-mesa-dev
# ---------------------------------------------------------------------------
find_package(CycloneDDS REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenGL REQUIRED)
find_package(glfw3 3.3 QUIET)
if(NOT glfw3_FOUND)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET glfw3)
  add_library(glfw ALIAS PkgConfig::GLFW)
endif()

# ---------------------------------------------------------------------------
# DDS types: idl/messages.idl -> messages.h / messages.c (made in build/)
# ---------------------------------------------------------------------------
# Works with Cyclone DDS 0.8 (Ubuntu 22.04) and 0.10 (Ubuntu 24.04).
if(COMMAND idlc_generate)
  idlc_generate(TARGET ba_messages FILES idl/messages.idl)
else()
  # Some packages have no CMake helper, so run idlc ourselves.
  find_program(IDLC idlc REQUIRED)
  set(_gen ${CMAKE_CURRENT_BINARY_DIR}/generated)
  file(MAKE_DIRECTORY ${_gen})
  add_custom_command(
    OUTPUT ${_gen}/messages.c ${_gen}/messages.h
    COMMAND ${IDLC} -l c ${CMAKE_CURRENT_SOURCE_DIR}/idl/messages.idl
    WORKING_DIRECTORY ${_gen}
    DEPENDS idl/messages.idl
    COMMENT "idlc -l c messages.idl")
  add_library(ba_messages STATIC ${_gen}/messages.c)
  target_include_directories(ba_messages PUBLIC ${_gen})
  target_link_libraries(ba_messages PUBLIC CycloneDDS::ddsc)
endif()

# Code used by every program
add_library(ba_common STATIC
  src/common/util.c
  src/common/dds_setup.c)
target_include_directories(ba_common PUBLIC src/common)
target_link_libraries(ba_common PUBLIC ba_messages CycloneDDS::ddsc Threads::Threads m)

# Game rules (no DDS calls), used by the server and the tests
add_library(ba_game STATIC src/server/game.c)
target_include_directories(ba_game PUBLIC src/server)
target_link_libraries(ba_game PUBLIC ba_common)

# Client networking, used by the client.
# interp.c has no DDS either, so it is tested too.
add_library(ba_client_net STATIC src/client/client_net.c src/client/interp.c)
target_include_directories(ba_client_net PUBLIC src/client)
target_link_libraries(ba_client_net PUBLIC ba_common)

# ---------------------------------------------------------------------------
# Programs
# ---------------------------------------------------------------------------
add_executable(ba_server src/server/server_main.c)
target_link_libraries(ba_server PRIVATE ba_game)

add_executable(ba_client src/client/client_main.c src/client/render.c)
target_include_directories(ba_client SYSTEM PRIVATE third_party)   # no warnings from 3rd-party code
target_link_libraries(ba_client PRIVATE ba_client_net glfw OpenGL::GL)

# ---------------------------------------------------------------------------
# Tests: ctest --test-dir build --output-on-failure
# ---------------------------------------------------------------------------
enable_testing()
add_executable(test_game tests/test_game.c)
target_link_libraries(test_game PRIVATE ba_game)
add_test(NAME game_rules COMMAND test_game)

add_executable(test_interp tests/test_interp.c)
target_link_libraries(test_interp PRIVATE ba_client_net)
add_test(NAME interpolation COMMAND test_interp)
