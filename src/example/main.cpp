// =============================================================================
// gcrecomp — Example Launcher
// =============================================================================
//
// This demonstrates how to use the gcrecomp toolkit to run recompiled
// GameCube code. In a real project, you would:
//
//   1. Recompile your DOL with the gcrecomp recompiler tool
//   2. Link the generated .cpp files with the gcrecomp runtime libraries
//   3. Call the game's entry point function
//
// This example just initializes all subsystems and runs an empty main loop
// to verify everything links and starts correctly.
//
// =============================================================================

#include "gcrecomp/runtime.h"
#include "gcrecomp/gx/gx.h"
#include "gcrecomp/audio/audio.h"
#include "gcrecomp/input/input.h"
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace gcrecomp;

static const uint32_t WINDOW_WIDTH  = 1280;
static const uint32_t WINDOW_HEIGHT = 720;

static void print_banner() {
    printf("\n");
    printf("    ____  _____ ____  ___  ___ ___  __  __ ____\n");
    printf("   / ___||  ___| __ )| __|/ __/ _ \\|  \\/  |  _ \\\n");
    printf("  | |  _ | |   |  _ \\| _|| (_| | | | |\\/| | |_) |\n");
    printf("  | |_| || |___| |_) | |___\\__\\_| |_| |  | |  __/\n");
    printf("   \\____||_____|____/|_____|   \\___/|_|  |_|_|\n");
    printf("\n");
    printf("   GameCube Static Recompilation Toolkit\n");
    printf("   Example Launcher — Subsystem Test\n");
    printf("\n");
}

int main(int argc, char** argv) {
    print_banner();

    // ---- Initialize Runtime ----
    printf("[*] Initializing runtime...\n");
    if (!runtime_init()) {
        fprintf(stderr, "Failed to initialize runtime\n");
        return 1;
    }

    // ---- Configure for your game ----
    // Each game needs its own GameConfig with the correct game ID.
    // This sets up the Dolphin OS low-memory state that the game reads at boot.
    GameConfig config;
    memcpy(config.game_id, "TEST", 4);      // Replace with your game's ID
    memcpy(config.company_code, "01", 2);   // "01" = Nintendo
    init_low_memory(&g_mem, config);
    register_os_functions();

    // ---- Initialize Graphics ----
    printf("[*] Initializing graphics (D3D11)...\n");
    gx::GXInit();
    if (!gx::GXInitBackend(nullptr, WINDOW_WIDTH, WINDOW_HEIGHT)) {
        fprintf(stderr, "Failed to initialize D3D11 backend\n");
        runtime_shutdown();
        return 1;
    }

    // ---- Initialize Audio ----
    printf("[*] Initializing audio...\n");
    audio::audio_init(32000, 2);

    // ---- Initialize Input ----
    printf("[*] Initializing input...\n");
    input::input_init();

    // ---- Main Loop ----
    printf("[*] All subsystems initialized successfully!\n");
    printf("[*] In a real project, you would now call the recompiled game's entry point.\n");
    printf("[*] Press ESC to exit.\n\n");

    bool running = true;

#ifdef _WIN32
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        input::input_update();
        audio::audio_update();

        // In a real project:
        //   game_entry_point(&g_ctx, &g_mem);
        // or:
        //   g_func_table.call(dol_entry_address, &g_ctx, &g_mem);

        gx::GXPresent();
    }
#endif

    // ---- Cleanup ----
    printf("\n[*] Shutting down...\n");
    input::input_shutdown();
    audio::audio_shutdown();
    gx::GXShutdownBackend();
    runtime_shutdown();

    printf("[*] gcrecomp example finished. All systems nominal.\n");
    return 0;
}
