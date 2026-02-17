# ──────────────────────────────────────────────────────────────────────────────
# NoiseGuard - CMake build for C dependencies (PortAudio + RNNoise)
#
# This CMake file fetches and builds PortAudio and RNNoise as static libraries.
# The resulting libs and headers are consumed by binding.gyp (node-gyp) to
# build the final .node addon.
#
# Usage (called by scripts/build-native.ps1):
#   cmake -S native -B deps/build -DCMAKE_BUILD_TYPE=Release
#   cmake --build deps/build --config Release
# ──────────────────────────────────────────────────────────────────────────────

cmake_minimum_required(VERSION 3.20)
project(noiseguard_deps LANGUAGES C CXX)

include(FetchContent)

# ── PortAudio ────────────────────────────────────────────────────────────────
# Build as static lib. Enable WASAPI on Windows.
set(PA_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(PA_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(PA_USE_WASAPI ON CACHE BOOL "" FORCE)
set(PA_USE_WDMKS OFF CACHE BOOL "" FORCE)
set(PA_USE_DS OFF CACHE BOOL "" FORCE)
set(PA_USE_WMME ON CACHE BOOL "" FORCE)
set(PA_USE_ASIO OFF CACHE BOOL "" FORCE)
set(PA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(PA_BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  portaudio
  GIT_REPOSITORY https://github.com/PortAudio/portaudio.git
  GIT_TAG        v19.7.0
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(portaudio)

# ── RNNoise ──────────────────────────────────────────────────────────────────
# Use the CMake-ready fork.
FetchContent_Declare(
  rnnoise
  GIT_REPOSITORY https://github.com/xiph/rnnoise.git
  GIT_TAG        master
  GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(rnnoise)
if(NOT rnnoise_POPULATED)
  FetchContent_Populate(rnnoise)

  # RNNoise doesn't have a CMakeLists.txt, so we build it manually.
  # Gather all .c source files from the rnnoise src directory.
  file(GLOB RNNOISE_SOURCES "${rnnoise_SOURCE_DIR}/src/*.c")

  add_library(rnnoise STATIC ${RNNOISE_SOURCES})
  target_include_directories(rnnoise PUBLIC "${rnnoise_SOURCE_DIR}/include")

  # RNNoise needs these defines for the bundled model weights.
  target_compile_definitions(rnnoise PRIVATE
    HAVE_CONFIG_H=0
    COMPILE_OPUS=0
  )

  # Suppress warnings in third-party code.
  if(MSVC)
    target_compile_options(rnnoise PRIVATE /W0)
  else()
    target_compile_options(rnnoise PRIVATE -w)
  endif()
endif()

# ── Install targets so binding.gyp can find them ─────────────────────────────
# Headers and libs go to deps/install/{include,lib}
set(INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/../deps/install" CACHE PATH "Install prefix")

install(TARGETS portaudio_static
  ARCHIVE DESTINATION "${INSTALL_PREFIX}/lib"
  LIBRARY DESTINATION "${INSTALL_PREFIX}/lib"
)

install(TARGETS rnnoise
  ARCHIVE DESTINATION "${INSTALL_PREFIX}/lib"
  LIBRARY DESTINATION "${INSTALL_PREFIX}/lib"
)

# Copy headers.
install(DIRECTORY "${portaudio_SOURCE_DIR}/include/"
  DESTINATION "${INSTALL_PREFIX}/include/portaudio"
  FILES_MATCHING PATTERN "*.h"
)

install(DIRECTORY "${rnnoise_SOURCE_DIR}/include/"
  DESTINATION "${INSTALL_PREFIX}/include/rnnoise"
  FILES_MATCHING PATTERN "*.h"
)

# Also copy WASAPI-specific header from PortAudio.
install(FILES "${portaudio_SOURCE_DIR}/src/hostapi/wasapi/pa_win_wasapi.h"
  DESTINATION "${INSTALL_PREFIX}/include/portaudio"
  OPTIONAL
)
