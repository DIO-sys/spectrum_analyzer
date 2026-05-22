cmake_minimum_required(VERSION 3.16)
project(spectrum_analyzer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── Dependencies ────────────────────────────────────────────────────────────

# libbladeRF
find_path(BLADERF_INCLUDE_DIR libbladeRF.h
    HINTS /usr/include /usr/local/include)
find_library(BLADERF_LIBRARY
    NAMES bladeRF
    HINTS /usr/lib /usr/local/lib)
if(NOT BLADERF_INCLUDE_DIR OR NOT BLADERF_LIBRARY)
    message(FATAL_ERROR "libbladeRF not found. Install with: sudo apt install libbladerf-dev")
endif()

# FFTW3 (single-precision float: fftw3f)
find_path(FFTW3_INCLUDE_DIR fftw3.h
    HINTS /usr/include /usr/local/include)
find_library(FFTW3_LIBRARY
    NAMES fftw3f
    HINTS /usr/lib /usr/local/lib)
if(NOT FFTW3_INCLUDE_DIR OR NOT FFTW3_LIBRARY)
    message(FATAL_ERROR "FFTW3 not found. Install with: sudo apt install libfftw3-dev")
endif()

# Dear ImGui (source build — clone into third_party/imgui)
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
if(NOT EXISTS ${IMGUI_DIR}/imgui.h)
    message(FATAL_ERROR
        "ImGui not found at ${IMGUI_DIR}.\n"
        "Run: git clone https://github.com/ocornut/imgui third_party/imgui")
endif()

# ImPlot (source build — clone into third_party/implot)
set(IMPLOT_DIR ${CMAKE_SOURCE_DIR}/third_party/implot)
if(NOT EXISTS ${IMPLOT_DIR}/implot.h)
    message(FATAL_ERROR
        "ImPlot not found at ${IMPLOT_DIR}.\n"
        "Run: git clone https://github.com/epezent/implot third_party/implot")
endif()

# ─── ImGui + ImPlot static library ───────────────────────────────────────────

add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    # Backend: OpenGL3 + GLFW (most portable desktop combo)
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    ${IMPLOT_DIR}/implot.cpp
    ${IMPLOT_DIR}/implot_items.cpp
)

target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMPLOT_DIR}
)

find_package(OpenGL REQUIRED)
find_package(glfw3  REQUIRED)
target_link_libraries(imgui PUBLIC OpenGL::GL glfw)

# ─── Main executable ─────────────────────────────────────────────────────────

add_executable(spectrum_analyzer
    src/main.cpp
    src/circular_buffer.cpp
    src/capture.cpp
    src/processing.cpp
    src/display.cpp
)

target_include_directories(spectrum_analyzer PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${BLADERF_INCLUDE_DIR}
    ${FFTW3_INCLUDE_DIR}
)

target_link_libraries(spectrum_analyzer PRIVATE
    ${BLADERF_LIBRARY}    # libbladeRF  — radio hardware
    ${FFTW3_LIBRARY}      # fftw3f      — FFT engine (float precision)
    imgui                 # Dear ImGui + ImPlot + backends
    pthread               # std::thread backing
)

# ─── Build type defaults ─────────────────────────────────────────────────────

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ─── Compiler flags ──────────────────────────────────────────────────────────

# Flags that always apply regardless of build type
target_compile_options(spectrum_analyzer PRIVATE
    -Wall
    -Wextra
    -Werror               # warnings are errors — no silent issues
    -march=native         # AVX/SSE for FFTW
)

# Release: optimize
target_compile_options(spectrum_analyzer PRIVATE
    $<$<CONFIG:Release>:-O2>
)

# Debug: no optimize, full debug info
target_compile_options(spectrum_analyzer PRIVATE
    $<$<CONFIG:Debug>:-O0>
    $<$<CONFIG:Debug>:-g3>
)

# ─── Sanitizer targets ───────────────────────────────────────────────────────
# Build with:  cmake -DCMAKE_BUILD_TYPE=Debug -DSANITIZE=address ..
#              cmake -DCMAKE_BUILD_TYPE=Debug -DSANITIZE=thread  ..
#
# Never mix address + thread — they are mutually exclusive.

set(SANITIZE "" CACHE STRING "Sanitizer to enable: address | thread | (empty)")

if(SANITIZE STREQUAL "address")
    message(STATUS "AddressSanitizer enabled")
    target_compile_options(spectrum_analyzer PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options   (spectrum_analyzer PRIVATE -fsanitize=address)

elseif(SANITIZE STREQUAL "thread")
    message(STATUS "ThreadSanitizer enabled")
    target_compile_options(spectrum_analyzer PRIVATE -fsanitize=thread  -fno-omit-frame-pointer)
    target_link_options   (spectrum_analyzer PRIVATE -fsanitize=thread)

elseif(NOT SANITIZE STREQUAL "")
    message(FATAL_ERROR "Unknown sanitizer '${SANITIZE}'. Use 'address' or 'thread'.")
endif()