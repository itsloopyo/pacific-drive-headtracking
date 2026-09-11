// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#include "ue4.h"

// Xbox Game Pass / Microsoft Store (GDK) build profiles. A store variant is a
// separate binary, not a label on a shared one: the GDK exe is
// PenDriverPro\Binaries\WinGDK\PenDriverPro-WinGDK-Shipping.exe, it is larger
// than the Steam exe and every RVA below differs. Append-only, same as
// steam_offsets.cpp.
namespace pdht::ue4 {

// Package KeplerInteractive.PacificDrive 1.15.1.0, fingerprint read off the
// running process on 2026-09-11.
//
// Derivation note, because it is different here: the GDK exe cannot be read off
// disk. Its DACL carries a conditional ACE granting read only to a process
// whose SYSAPPID is the package identity, so every other file in that directory
// opens and the game exe returns access-denied - the module has to be dumped
// out of the running process instead. Every RVA below was then derived twice,
// once by porting the Steam signature and once by an independent route, and the
// independent route was run against the Steam build first to check it
// reproduced the RVAs already pinned there.
extern const BuildProfile kGdkProfile_20260911 = {
    "gdk-wingdk-20260911",
    {0x698D6409u, 0x05B40000u, 0x0588E2DFu},  // TimeDateStamp, SizeOfImage, CheckSum
    // APlayerCameraManager::CameraCachePrivate reflects to +0x1AE0 on this
    // build exactly as it does on Steam, read out of the generated
    // FPropertyParams record rather than assumed to carry over, so POV sits at
    // +0x1AF0 and its Rotation 12 bytes past that.
    0x1AFC,  // povRotationOffset
    // ULocalPlayer::GetProjectionData, slot 89 of the 92-entry ULocalPlayer
    // vtable at rva 0x46a8b20. The chain that names that vtable is intact on
    // this build: GetPrivateStaticClass at 0x3129860 passes L"/Script/Engine",
    // L"LocalPlayer" and 0x258 (sizeof(ULocalPlayer) == 600) alongside
    // InternalConstructor 0x3129b10, which tail-jumps the default constructor
    // 0x2c7aa90, whose one and only vtable store is 0x46a8b20.
    0x2c83c30,  // getProjectionDataRva
    // UWidget::SetRenderTranslation and UWidgetLayoutLibrary::GetViewportScale,
    // taken from the native registration table: the {name, exec} entry that
    // points at the ANSI function name is followed by the exec thunk, and the
    // last call before the thunk's epilogue is the native.
    0x26d40c0,  // setRenderTranslationRva
    0x269dbc0,  // getViewportScaleRva
    // UUserWidget::SetPositionInViewport(FVector2D, bool), same route.
    0x26d3b00,  // setPositionInViewportRva
};

}  // namespace pdht::ue4
