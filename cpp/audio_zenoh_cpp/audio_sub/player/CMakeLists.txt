cmake_minimum_required(VERSION 3.16)
project(audio_player LANGUAGES CXX)

# -----------------------------------------------------------------------------
#  Real-time Opus-over-Zenoh audio PLAYER (live playback, minimum latency).
#
#  Build (from this directory):
#       mkdir build && cd build
#       cmake ..
#       make
#       ./audio_player          # reads ../../audio_pub/config.cfg by default
#
#  Same dependency set as the publisher: Zenoh C++, PortAudio, Opus.
# -----------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")
add_compile_options(-Wall -Wextra)

# ---- Zenoh C++ (header-only wrapper over libzenohc) -------------------------
find_package(zenohc REQUIRED)
find_package(zenohcxx REQUIRED)

# ---- PortAudio (speaker output), via pkg-config -----------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(PORTAUDIO REQUIRED IMPORTED_TARGET portaudio-2.0)

# ---- Opus codec -------------------------------------------------------------
# The Opus dev files on this machine live under the Anaconda prefix, so search
# there (and the usual system locations) explicitly. Override on the cmake line
# with -DOPUS_ROOT=/path if needed.
find_path(OPUS_INCLUDE_DIR opus/opus.h
  HINTS
    $ENV{OPUS_ROOT}/include
    $ENV{CONDA_PREFIX}/include
    /home/rana/anaconda3/include
    /usr/include
    /usr/local/include)
find_library(OPUS_LIBRARY
  NAMES opus
  HINTS
    $ENV{OPUS_ROOT}/lib
    $ENV{CONDA_PREFIX}/lib
    /home/rana/anaconda3/lib
    /usr/lib
    /usr/lib/x86_64-linux-gnu
    /usr/local/lib)

if(NOT OPUS_INCLUDE_DIR OR NOT OPUS_LIBRARY)
  message(FATAL_ERROR
    "Opus not found. Set -DOPUS_ROOT=<prefix> (expects <prefix>/include/opus/opus.h "
    "and <prefix>/lib/libopus). Found include='${OPUS_INCLUDE_DIR}' lib='${OPUS_LIBRARY}'.")
endif()
message(STATUS "Opus include: ${OPUS_INCLUDE_DIR}")
message(STATUS "Opus library: ${OPUS_LIBRARY}")

# -----------------------------------------------------------------------------
add_executable(audio_player
  src/main.cpp)

target_include_directories(audio_player PRIVATE
  ${OPUS_INCLUDE_DIR})

target_link_libraries(audio_player PRIVATE
  zenohcxx::zenohc
  PkgConfig::PORTAUDIO
  ${OPUS_LIBRARY})

# Embed the Opus (and any non-standard) library directory as an RPATH so the
# binary runs without setting LD_LIBRARY_PATH.
get_filename_component(OPUS_LIB_DIR "${OPUS_LIBRARY}" DIRECTORY)
set_target_properties(audio_player PROPERTIES
  BUILD_RPATH "${OPUS_LIB_DIR};/usr/local/lib"
  INSTALL_RPATH "${OPUS_LIB_DIR};/usr/local/lib")
