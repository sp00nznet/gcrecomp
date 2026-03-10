// =============================================================================
// PPC Context - Global CPU State
//
// This file instantiates the single global PPCContext that all recompiled
// GameCube code operates on. Every recompiled function receives a pointer
// to this context and reads/writes its registers (r0-r31, f0-f31, CR, LR,
// etc.) to emulate the original PowerPC instruction behavior.
//
// The context is zeroed at startup by runtime_init() via PPCContext::reset().
// The DOL loader then sets r1 (stack pointer), r2 (SDA2 base), and r13
// (SDA base) before calling the game's entry point.
// =============================================================================

#include "gcrecomp/runtime.h"

namespace gcrecomp {

PPCContext g_ctx;

} // namespace gcrecomp
