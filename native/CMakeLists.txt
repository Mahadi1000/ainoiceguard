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

# Allow FetchContent subprojects (e.g. PortAudio) that use cmake_minimum_required(< 3.5)
# when using CMake 4.x, which removed compatibility with older version requirements.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

# Set install prefix BEFORE FetchContent so PortAudio's install() uses our local path.
# Default to deps/install relative to project root if not set via command line.
if(NOT CMAKE_INSTALL_PREFIX OR CMAKE_INSTALL_PREFIX STREQUAL "")
  get_filename_component(PROJECT_ROOT "${CMAKE_SOURCE_DIR}" DIRECTORY)
  set(CMAKE_INSTALL_PREFIX "${PROJECT_ROOT}/deps/install" CACHE PATH "Install prefix" FORCE)
endif()

include(FetchContent)

# ── PortAudio ────────────────────────────────────────────────────────────────
# Build as static lib. On Windows enable WASAPI; on Linux/macOS use default (ALSA/CoreAudio).
set(PA_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(PA_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(PA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(PA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
if(WIN32)
  set(PA_USE_WASAPI ON CACHE BOOL "" FORCE)
  set(PA_USE_WDMKS OFF CACHE BOOL "" FORCE)
  set(PA_USE_DS OFF CACHE BOOL "" FORCE)
  set(PA_USE_WMME ON CACHE BOOL "" FORCE)
  set(PA_USE_ASIO OFF CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(
  portaudio
  GIT_REPOSITORY https://github.com/PortAudio/portaudio.git
  GIT_TAG        v19.7.0
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(portaudio)

# ── RNNoise ──────────────────────────────────────────────────────────────────
# Use the CMake-ready fork.
# Use Mumble's fork: MSVC-friendly (USE_MALLOC for VLAs), same public API (rnnoise.h).
FetchContent_Declare(
  rnnoise
  GIT_REPOSITORY https://github.com/mumble-voip/rnnoise.git
  GIT_TAG        master
  GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(rnnoise)
if(NOT rnnoise_POPULATED)
  FetchContent_Populate(rnnoise)

  file(GLOB RNNOISE_SOURCES "${rnnoise_SOURCE_DIR}/src/*.c")

  add_library(rnnoise STATIC ${RNNOISE_SOURCES})
  target_include_directories(rnnoise
    PUBLIC "${rnnoise_SOURCE_DIR}/include"
    PRIVATE "${rnnoise_SOURCE_DIR}/src"
  )

  # Do not define HAVE_CONFIG_H so that #ifdef HAVE_CONFIG_H in source skips config.h.
  # COMPILE_OPUS=0: use bundled model.
  # USE_MALLOC: use malloc instead of VLAs (required for MSVC; Mumble fork supports this).
  target_compile_definitions(rnnoise PRIVATE
    COMPILE_OPUS=0
    USE_MALLOC
  )

  # MSVC: expose M_PI from math.h and suppress warnings.
  if(MSVC)
    target_compile_definitions(rnnoise PRIVATE _USE_MATH_DEFINES)
    target_compile_options(rnnoise PRIVATE /W0)
  else()
    target_compile_options(rnnoise PRIVATE -w)
  endif()
endif()

# ── Install targets so binding.gyp can find them ─────────────────────────────
# Headers and libs go to CMAKE_INSTALL_PREFIX/{include,lib} (set above).
# PortAudio's install() commands will use CMAKE_INSTALL_PREFIX automatically.
# We add our own install() for RNNoise headers and to ensure PortAudio libs are copied.

install(TARGETS portaudio_static
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
)

install(TARGETS rnnoise
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
)

# Copy headers (PortAudio's install already handles its headers, but we ensure RNNoise).
install(DIRECTORY "${rnnoise_SOURCE_DIR}/include/"
  DESTINATION include/rnnoise
  FILES_MATCHING PATTERN "*.h"
)

# Windows: copy WASAPI header for the addon (if PortAudio's install missed it).
if(WIN32)
  install(FILES "${portaudio_SOURCE_DIR}/src/hostapi/wasapi/pa_win_wasapi.h"
    DESTINATION include
    OPTIONAL
  )
endif()
