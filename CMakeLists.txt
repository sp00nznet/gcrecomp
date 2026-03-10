cmake_minimum_required(VERSION 3.20)
project(gcrecomp VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# =============================================================================
# gcrecomp — GameCube Static Recompilation Toolkit
#
# This CMake file builds the reusable toolkit libraries and tools.
# Game-specific projects link against these as static libraries.
#
# Targets:
#   gcrecomp_runtime    — CPU context, memory, function table, OS HLE
#   gcrecomp_gx         — GX graphics (TEV, textures, D3D11 backend)
#   gcrecomp_audio      — DSP ADPCM decoder, voice mixer
#   gcrecomp_input      — Controller input (keyboard, XInput)
#   gcrecomp_recompiler — PPC → C static recompiler tool (when available)
# =============================================================================

if(WIN32)
    add_definitions(-DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS)
endif()

# Build options
option(GCRECOMP_BUILD_TOOLS "Build recompiler tools" ON)
option(GCRECOMP_BUILD_RUNTIME "Build the game runtime libraries" ON)
option(GCRECOMP_BUILD_EXAMPLE "Build the example launcher" ON)

# ===========================================================================
# Runtime Library: Links with recompiled game code
# ===========================================================================
if(GCRECOMP_BUILD_RUNTIME)
    # Core runtime (CPU context, memory, OS HLE)
    add_library(gcrecomp_runtime STATIC
        src/runtime/ppc_context.cpp
        src/runtime/memory.cpp
        src/runtime/func_table.cpp
        src/os/os_funcs.cpp
    )
    target_include_directories(gcrecomp_runtime PUBLIC include)

    # GX Graphics (D3D11 backend)
    add_library(gcrecomp_gx STATIC
        src/gx/gx_core.cpp
        src/gx/gx_tev.cpp
        src/gx/gx_texture.cpp
        src/gx/gx_vertex.cpp
        src/gx/gx_matrix.cpp
        src/gx/gx_display_list.cpp
        src/gx/d3d11/d3d11_backend.cpp
        src/gx/d3d11/d3d11_shaders.cpp
    )
    target_include_directories(gcrecomp_gx PUBLIC include)
    if(WIN32)
        target_link_libraries(gcrecomp_gx PRIVATE d3d11 d3dcompiler dxgi)
    endif()

    # Audio (DSP ADPCM + mixer)
    add_library(gcrecomp_audio STATIC
        src/audio/dsp_decoder.cpp
        src/audio/audio_mixer.cpp
        src/audio/audio_system.cpp
    )
    target_include_directories(gcrecomp_audio PUBLIC include)
    if(WIN32)
        target_link_libraries(gcrecomp_audio PRIVATE xaudio2)
    endif()

    # Input (keyboard + XInput)
    add_library(gcrecomp_input STATIC
        src/input/input_system.cpp
        src/input/pad_mapping.cpp
    )
    target_include_directories(gcrecomp_input PUBLIC include)
    if(WIN32)
        target_link_libraries(gcrecomp_input PRIVATE xinput)
    endif()
endif()

# ===========================================================================
# Example Launcher (shows how to use gcrecomp in a game project)
# ===========================================================================
if(GCRECOMP_BUILD_EXAMPLE AND GCRECOMP_BUILD_RUNTIME)
    add_executable(gcrecomp_example
        src/example/main.cpp
    )
    target_link_libraries(gcrecomp_example PRIVATE
        gcrecomp_runtime gcrecomp_gx gcrecomp_audio gcrecomp_input
    )
    target_include_directories(gcrecomp_example PRIVATE include)
    if(WIN32)
        target_link_libraries(gcrecomp_example PRIVATE d3d11 d3dcompiler dxgi)
    endif()
endif()
