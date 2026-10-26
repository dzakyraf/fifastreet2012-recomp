#pragma once
// The guest addresses the runtime names for FIFA Street (Xbox 360). The runtime
// hooks the title's own functions by address; a hook whose address is not
// defined here is compiled out, and the generic kernel, GPU and audio layers run
// without it.
//
// FIFA Street is two modules (ppc/ppc_modules.h): default.xex, a launcher that
// loads fifadllzf.xex.dll and calls its export 1, and the DLL, which is the game.
// A function entry is the bare hex address, without 0x, so GUEST_HOOK can paste
// it onto sub_ / __imp__sub_ / 0x.

#define PES17_TITLE_NAME  "FIFA Street"
#define T_ENTRY_POINT   0x820FF498u          // default.xex's ENTRY_POINT
#define T_DLL_ENTRY     0x82737CE0u          // fifadllzf.xex.dll's ENTRY_POINT (DllMain)

// ---- still to be located ---------------------------------------------------
// The PES 2017 hooks (D3D Present, BlockOnFence, ArenaWait, the 16:9 aspect
// floats) are left undefined: their addresses are PES's, and the D3D9 the DLL
// links statically has to be searched for them again.
// #define T_D3D_Present          ........
// #define T_D3D_BlockOnFence     ........

// D3D's flip scheduling (`sub_8270F040(flip word, ...)`): the present interval is bits 8-11 of
// r3, and the flip is queued for the blank `last flip + interval` (the vblank handler,
// sub_8270EF10, retires it). FIFA Street asks for 2 -- 30 FPS -- in its menus and cinematics.
#define T_D3D_ScheduleFlip     8270F040
// The title's frame limiter (`sub_82613E70(timer, rate in Hz)`): spins on the timebase until a
// 1/rate period has passed. The render thread caps its cinematics at 40 (sub_82619AE0).
#define T_FrameLimiter         82613E70
// One pass of D3D's busy-wait on the GPU (`sub_826FCC30(waiter)`: 32 db16cyc then a poll; 1 while
// still waiting). D3D's fence wait (sub_82713710) calls it in a loop.
#define T_D3D_SpinOnce         826FCC30

// Data the diagnostics read; zero means "not derived for this title".
#define T_DATA_DeviceTable     0u
#define T_DATA_ClientStates    0u
#define T_CLIENT_VALID         0u
#define T_CLIENT_ORIGIN        0u
#define T_CLIENT_ANGLES        0u

// Hooking a recompiled function by address: GUEST_HOOK(T_x) { ... GUEST_ORIG(T_x)(ctx, base); }
#define PES17_CAT_(a, b) a##b
#define PES17_CAT(a, b) PES17_CAT_(a, b)
#define PES17_STR_(x) #x
#define PES17_STR(x) PES17_STR_(x)
#define GUEST_FUNC(name) PES17_CAT(sub_, name)          // the recompiled function
#define GUEST_ORIG(name) PES17_CAT(__imp__sub_, name)   // the original, once the function is hooked
#define GUEST_ADDR(name) PES17_CAT(0x, name)            // its guest address
#define GUEST_NAME(name) "sub_" PES17_STR(name)         // its name, for a log line
#define GUEST_HOOK(name) PPC_FUNC_IMPL(GUEST_ORIG(name)); PPC_FUNC(GUEST_FUNC(name))
