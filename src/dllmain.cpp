// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "mod.h"

namespace {

DWORD WINAPI Bootstrap(LPVOID /*param*/) {
    pdht::Mod::Get().Run();
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(hModule);
            // Pin the module, so FreeLibrary can never unmap it. That makes "an
            // ASI plugin is never unloaded" a property of the binary instead of
            // an assumption, and everything else here rests on it.
            //
            // The alternative - tearing down from DLL_PROCESS_DETACH - cannot be
            // made correct. It runs on the loader lock, and teardown has to join
            // three threads and reach WSACleanup(), which unloads any layered
            // Winsock provider and so takes the loader lock recursively from a
            // detach callback: the textbook deadlock, and a VPN or AV shim is
            // enough to trigger it. Removing the detours is no better - MinHook
            // rewinds a thread parked on the patched prologue, not one already
            // executing the detour body, so unmapping this image mid-frame
            // resumes the render thread on unmapped memory.
            HMODULE pinned = nullptr;
            GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(hModule), &pinned);
            HANDLE t = CreateThread(nullptr, 0, Bootstrap, nullptr, 0, nullptr);
            if (t) CloseHandle(t);
            break;
        }
        case DLL_PROCESS_DETACH:
            // Pinned, so this only ever fires with the process terminating, and
            // by then the OS has already killed every thread wherever it stood.
            // A worker killed inside log::Line still holds the log's mutex, so
            // any log call here blocks forever and the game never exits. Nothing
            // is lost by doing nothing: each log line is flushed as it is
            // written, the hooks die with the address space, and the OS reclaims
            // the sockets and threads.
            break;
        default:
            break;
    }
    return TRUE;
}
