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
option(GCRECOMP_FETCH_SDL3 "Download and build SDL3 with FetchContent instead of using an installed one" OFF)
# Unit tests (CTest): on when gcrecomp is the top-level project, off when it is added as a subdirectory.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    option(GCRECOMP_BUILD_TESTS "Build the unit tests" ON)
else()
    option(GCRECOMP_BUILD_TESTS "Build the unit tests" OFF)
endif()

# ===========================================================================
# Runtime Library: Links with recompiled game code
# ===========================================================================
if(GCRECOMP_BUILD_RUNTIME)
    # Core runtime (CPU context, memory, OS HLE)
    add_library(gcrecomp_runtime STATIC
        src/runtime/ppc_context.cpp
        src/runtime/memory.cpp
        src/runtime/func_table.cpp
        src/runtime/interrupts.cpp
        src/os/os_funcs.cpp
        src/os/yaz0.cpp
        src/os/rarc.cpp
        src/os/ai_funcs.cpp
        src/os/ar_funcs.cpp
        src/os/dsp_interface.cpp
        src/os/card_image.cpp
        src/os/card_funcs.cpp
        src/os/dvd_funcs.cpp
        src/os/gx_funcs.cpp
        src/os/pad_funcs.cpp
        src/os/thread_funcs.cpp
        src/os/threads.cpp
        src/os/vi.cpp
        src/os/rel.cpp
    )
    target_include_directories(gcrecomp_runtime PUBLIC include)
    find_package(Threads REQUIRED)
    target_link_libraries(gcrecomp_runtime PUBLIC Threads::Threads)
    if(WIN32)
        target_link_libraries(gcrecomp_runtime PUBLIC winmm)  # timeBeginPeriod
    endif()
    target_link_libraries(gcrecomp_runtime PUBLIC gcrecomp_audio)

    # GX Graphics (D3D11 backend)
    add_library(gcrecomp_gx STATIC
        src/gx/gx_core.cpp
        src/gx/gx_tev.cpp
        src/gx/gx_texture.cpp
        src/gx/gx_texobj.cpp
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
        src/audio/audio_sink.cpp
        src/audio/wav_sink.cpp
        src/audio/ax_hle.cpp
        src/audio/audio_interface.cpp
        src/audio/sdl_sink.cpp
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

    # SDL3 (audio output and the GP window/GL context). Resolved, in order, from:
    #   1. FetchContent, when -DGCRECOMP_FETCH_SDL3=ON (CI, or no system SDL3);
    #   2. find_package(SDL3 CONFIG) (vcpkg, SDL3 release archives, distro cmake packages);
    #   3. pkg-config (sdl3.pc).
    # Whatever is found is exposed as the INTERFACE target gcrecomp_sdl3.
    if(GCRECOMP_FETCH_SDL3)
        include(FetchContent)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        set(SDL_TESTS OFF CACHE BOOL "" FORCE)
        set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(SDL3
            GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
            GIT_TAG release-3.2.28
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(SDL3)
        set(GCRECOMP_SDL3_TARGET SDL3::SDL3-static)
    else()
        find_package(SDL3 CONFIG QUIET)
        if(TARGET SDL3::SDL3)
            set(GCRECOMP_SDL3_TARGET SDL3::SDL3)
        elseif(TARGET SDL3::SDL3-shared)
            set(GCRECOMP_SDL3_TARGET SDL3::SDL3-shared)
        else()
            find_package(PkgConfig QUIET)
            if(PkgConfig_FOUND)
                pkg_check_modules(SDL3_PC QUIET IMPORTED_TARGET sdl3)
                if(SDL3_PC_FOUND)
                    set(GCRECOMP_SDL3_TARGET PkgConfig::SDL3_PC)
                endif()
            endif()
        endif()
    endif()
    if(GCRECOMP_SDL3_TARGET)
        add_library(gcrecomp_sdl3 INTERFACE)
        target_link_libraries(gcrecomp_sdl3 INTERFACE ${GCRECOMP_SDL3_TARGET})
        # Audio output through SDL3 (the null sink is always available).
        target_compile_definitions(gcrecomp_audio PRIVATE GCRECOMP_HAVE_SDL3)
        target_link_libraries(gcrecomp_audio PUBLIC gcrecomp_sdl3)
        # GP: consumes the GX command stream written by the recompiled SDK and draws it with OpenGL 3.3.
        # The GL entry points are loaded at run time through SDL_GL_GetProcAddress (src/gp/gl_loader.*),
        # so no libGL / opengl32 is linked.
        add_library(gcrecomp_gp STATIC
            src/gp/gp.cpp
            src/gp/gp_parser.cpp
            src/gp/lighting.cpp
            src/gp/gl_renderer.cpp
            src/gp/gl_loader.cpp
            src/gp/input.cpp
            src/gp/input_script.cpp
            src/gp/efb_copy.cpp
            src/gp/gp_decode.cpp
        )
        target_include_directories(gcrecomp_gp PUBLIC include)
        # The texture decoders live in gcrecomp_gx.
        target_link_libraries(gcrecomp_gp PUBLIC gcrecomp_gx gcrecomp_sdl3)
        if(GCRECOMP_BUILD_TESTS)
            enable_testing()
            add_executable(gcrecomp_input_tests tests/input_tests.cpp)
            target_include_directories(gcrecomp_input_tests PRIVATE include src/gp)
            target_link_libraries(gcrecomp_input_tests PRIVATE gcrecomp_gp gcrecomp_runtime)
            add_test(NAME input COMMAND gcrecomp_input_tests)
            add_executable(input_script_test tests/input_script_test.cpp)
            target_include_directories(input_script_test PRIVATE include src/gp)
            target_link_libraries(input_script_test PRIVATE gcrecomp_gp gcrecomp_runtime)
            add_test(NAME input_script COMMAND input_script_test)
            add_executable(gp_decode_test tests/gp_decode_test.cpp)
            target_include_directories(gp_decode_test PRIVATE src/gp)
            target_link_libraries(gp_decode_test PRIVATE gcrecomp_gp)
            add_test(NAME gp_decode_test COMMAND gp_decode_test)
            # Needs a GL context: SDL_VIDEO_DRIVER=offscreen ./gp_render_check (not run by ctest).
            add_executable(gp_render_check tests/gp_render_check.cpp)
            target_include_directories(gp_render_check PRIVATE src/gp)
            target_link_libraries(gp_render_check PRIVATE gcrecomp_gp gcrecomp_runtime)
        endif()
    else()
        message(STATUS "SDL3 not found: gcrecomp_gp (GP renderer) is not built")
    endif()
endif()

# Unit tests (no display, no disc): ctest or build/tests/efb_copy_test
include(CTest)
if(BUILD_TESTING)
    add_executable(efb_copy_test tests/efb_copy_test.cpp src/gp/efb_copy.cpp src/gx/gx_texture.cpp)
    target_include_directories(efb_copy_test PRIVATE include src/gp)
    add_test(NAME efb_copy COMMAND efb_copy_test)
endif()

# ===========================================================================
# Unit tests
# ===========================================================================
if(GCRECOMP_BUILD_TESTS AND GCRECOMP_BUILD_RUNTIME)
    enable_testing()
    add_executable(test_threads tests/test_threads.cpp)
    target_link_libraries(test_threads PRIVATE gcrecomp_runtime)
    add_test(NAME threads COMMAND test_threads)
    set_tests_properties(threads PROPERTIES TIMEOUT 60)
endif()

# ===========================================================================
# Tests (no display, no disc): ctest --test-dir build
# ===========================================================================
option(GCRECOMP_BUILD_TESTS "Build the unit tests" ON)
if(GCRECOMP_BUILD_TESTS AND GCRECOMP_BUILD_RUNTIME)
    enable_testing()
    add_executable(gcrecomp_audio_tests tests/audio_tests.cpp)
    target_link_libraries(gcrecomp_audio_tests PRIVATE gcrecomp_audio)
    add_test(NAME audio COMMAND gcrecomp_audio_tests)
    add_executable(gcrecomp_ai_hle_tests tests/ai_hle_tests.cpp)
    target_link_libraries(gcrecomp_ai_hle_tests PRIVATE gcrecomp_runtime gcrecomp_audio)
    add_test(NAME ai_hle COMMAND gcrecomp_ai_hle_tests)
    add_executable(gcrecomp_dsp_tests tests/dsp_tests.cpp)
    target_link_libraries(gcrecomp_dsp_tests PRIVATE gcrecomp_runtime)
    add_test(NAME dsp COMMAND gcrecomp_dsp_tests)
    add_executable(gcrecomp_ax_tests tests/ax_tests.cpp)
    target_link_libraries(gcrecomp_ax_tests PRIVATE gcrecomp_audio)
    add_test(NAME ax COMMAND gcrecomp_ax_tests)
    # Manual probe of the host audio sink (not run by ctest): see tools/audio_probe/main.cpp.
    add_executable(gcrecomp_audio_probe tools/audio_probe/main.cpp)
    target_link_libraries(gcrecomp_audio_probe PRIVATE gcrecomp_audio)
endif()

# ===========================================================================
# Recompiler Tools: DOL parser, PPC disassembler, symbol map, DOL info
# ===========================================================================
if(GCRECOMP_BUILD_TOOLS)
    # Static library for shared tool code (DOL parser, disassembler, CFG, codegen, symbols)
    # Includes REL + Yaz0 from src/os so the recompiler can take a REL input.
    add_library(gcrecomp_tools STATIC
        tools/recompiler/dol_parser.cpp
        tools/recompiler/ppc_disasm.cpp
        tools/recompiler/cfg_builder.cpp
        tools/recompiler/ppc_to_c.cpp
        tools/recompiler/symbol_map.cpp
        src/os/rel.cpp
        src/os/yaz0.cpp
    )
    target_include_directories(gcrecomp_tools PUBLIC include)

    # DOL Info utility
    add_executable(gcrecomp_dol_info
        tools/dol_info/main.cpp
    )
    target_link_libraries(gcrecomp_dol_info PRIVATE gcrecomp_tools)
    target_include_directories(gcrecomp_dol_info PRIVATE include)

    # Static Recompiler (PPC -> C)
    add_executable(gcrecomp_recompiler
        tools/recompiler/main.cpp
    )
    target_link_libraries(gcrecomp_recompiler PRIVATE gcrecomp_tools)
    target_include_directories(gcrecomp_recompiler PRIVATE include)
    if(GCRECOMP_BUILD_TESTS)
        enable_testing()
        add_executable(recompiler_rc_test tests/recompiler_rc_test.cpp)
        target_link_libraries(recompiler_rc_test PRIVATE gcrecomp_tools)
        add_test(NAME recompiler_rc COMMAND recompiler_rc_test)
        if(GCRECOMP_BUILD_RUNTIME)
            # Floating-point / paired-single semantics of the emitted code: the generator runs the emitter at
            # build time, the test compiles and runs the result.
            add_executable(ps_semantics_gen tests/ps_semantics_gen.cpp)
            target_link_libraries(ps_semantics_gen PRIVATE gcrecomp_tools)
            add_custom_command(
                OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ps_semantics.inc
                COMMAND ps_semantics_gen ${CMAKE_CURRENT_BINARY_DIR}/ps_semantics.inc
                DEPENDS ps_semantics_gen tests/ps_semantics_cases.h)
            add_executable(ps_semantics_test tests/ps_semantics_test.cpp ${CMAKE_CURRENT_BINARY_DIR}/ps_semantics.inc)
            target_include_directories(ps_semantics_test PRIVATE include tests ${CMAKE_CURRENT_BINARY_DIR})
            target_link_libraries(ps_semantics_test PRIVATE gcrecomp_runtime)
            add_test(NAME ps_semantics COMMAND ps_semantics_test)
            # ppc-diff: the code the recompiler emits against a reference CPU (Unicorn), see tools/ppc-diff/README.md.
            # Needs a vectors file (tools/ppc-diff/gen_vectors.py), so it only builds when one is given.
            set(GCRECOMP_PPC_DIFF_VECTORS "" CACHE FILEPATH "Vectors file for the ppc_diff differential test")
            if(GCRECOMP_PPC_DIFF_VECTORS)
                add_executable(ppc_diff_gen tools/ppc-diff/ppc_diff_gen.cpp)
                target_link_libraries(ppc_diff_gen PRIVATE gcrecomp_tools)
                add_custom_command(
                    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ppc_diff_cases.inc
                    COMMAND ppc_diff_gen ${GCRECOMP_PPC_DIFF_VECTORS} ${CMAKE_CURRENT_BINARY_DIR}/ppc_diff_cases.inc
                    DEPENDS ppc_diff_gen ${GCRECOMP_PPC_DIFF_VECTORS})
                add_executable(ppc_diff_run tools/ppc-diff/ppc_diff_run.cpp ${CMAKE_CURRENT_BINARY_DIR}/ppc_diff_cases.inc)
                target_include_directories(ppc_diff_run PRIVATE include ${CMAKE_CURRENT_BINARY_DIR})
                target_link_libraries(ppc_diff_run PRIVATE gcrecomp_runtime)
                add_test(NAME ppc_diff COMMAND ppc_diff_run)
            endif()
        endif()
    endif()
endif()

# ===========================================================================
# Example Launcher (shows how to use gcrecomp in a game project). It uses the D3D11 gx backend, so it is
# Windows-only; on other platforms it does not link (the backend is empty).
# ===========================================================================
if(GCRECOMP_BUILD_EXAMPLE AND GCRECOMP_BUILD_RUNTIME AND WIN32)
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

# ===========================================================================
# Unit tests (no display, no disc): cmake -DGCRECOMP_BUILD_TESTS=ON, then ctest
# ===========================================================================
if(GCRECOMP_BUILD_TESTS)
    enable_testing()
    add_executable(test_gp_lighting tests/test_gp_lighting.cpp src/gp/lighting.cpp)
    target_include_directories(test_gp_lighting PRIVATE src/gp)
    add_test(NAME gp_lighting COMMAND test_gp_lighting)
    add_executable(card_image_test tests/card_image_test.cpp src/os/card_image.cpp)
    target_include_directories(card_image_test PRIVATE src/os)
    add_test(NAME card_image COMMAND card_image_test)
    if(GCRECOMP_BUILD_RUNTIME)
        # Parser and vertex decoder against a recording renderer: no SDL, GL or disc.
        add_executable(gp_parser_test tests/gp_parser_test.cpp src/gp/gp_parser.cpp src/gp/lighting.cpp)
        target_include_directories(gp_parser_test PRIVATE include src/gp)
        target_link_libraries(gp_parser_test PRIVATE gcrecomp_runtime)
        add_test(NAME gp_parser COMMAND gp_parser_test)
        add_executable(card_hle_test tests/card_hle_test.cpp)
        target_link_libraries(card_hle_test PRIVATE gcrecomp_runtime)
        add_test(NAME card_hle COMMAND card_hle_test)
        add_executable(dvd_hle_test tests/dvd_hle_test.cpp)
        target_link_libraries(dvd_hle_test PRIVATE gcrecomp_runtime)
        add_test(NAME dvd_hle COMMAND dvd_hle_test)
        add_executable(os_sound_mode_test tests/os_sound_mode_test.cpp)
        target_link_libraries(os_sound_mode_test PRIVATE gcrecomp_runtime)
        add_test(NAME os_sound_mode COMMAND os_sound_mode_test)
    endif()
endif()
