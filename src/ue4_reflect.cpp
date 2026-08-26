// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#include "ue4.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>
#include <string>

#include "cameraunlock/logging/file_log.h"
#include "cameraunlock/unreal/ue_runtime.h"

// UE4.27 reflection over the live process. The chunked FUObjectArray and the
// FNamePool block format are identical between UE 4.23+ and UE5, so the core
// cameraunlock::unreal helpers (ForEachUObject / ClassName / ResolveFName) work
// unchanged once given the UE4.27 layout. We discover the two global addresses
// (GUObjectArray, FNamePool) by brute-force scanning the module's writable data
// rather than pinning RVAs: it needs no per-build offsets and re-discovers
// itself after a patch. Self-validation gates every step, so a wrong guess
// finds nothing and the mod stays dormant. The camera POV is read as floats -
// the core's SafeReadFVector is UE5 doubles and must not be used here.
namespace pdht::ue4 {

namespace un = cameraunlock::unreal;
namespace log = cameraunlock::logging;

namespace {

// Fixed UE4.27 struct offsets (engine-version-stable).
constexpr std::size_t kNumElementsInChunked = 0x14;
constexpr std::size_t kMaxElementsInChunked = 0x10;
constexpr std::size_t kFUObjectItemSize = 0x18;
constexpr std::size_t kChunkNumElems = 65536;
constexpr std::size_t kFNamePoolBlocks = 0x10;
constexpr std::size_t kClassPrivate = 0x10;
constexpr std::size_t kNamePrivate = 0x18;
constexpr std::size_t kOuterPrivate = 0x20;
constexpr std::size_t kObjectFlags = 0x08;
constexpr std::size_t kInternalIndex = 0x0C;
constexpr std::size_t kFUObjectItemFlags = 0x08;
// EObjectFlags
constexpr std::uint32_t kFlagClassDefaultObject = 0x00000010;
constexpr std::uint32_t kFlagArchetypeObject = 0x00000020;
constexpr std::uint32_t kFlagWasLoaded = 0x00080000;
// EInternalObjectFlags
constexpr std::uint32_t kInternalUnreachable = 1u << 28;
constexpr std::uint32_t kInternalPendingKill = 1u << 29;
// UStruct and the FField/FProperty hierarchy that UE 4.25 split properties into.
constexpr std::size_t kSuperStruct = 0x40;
constexpr std::size_t kChildProperties = 0x50;
constexpr std::size_t kFieldNext = 0x20;
constexpr std::size_t kFieldName = 0x28;
constexpr std::size_t kPropertyOffsetInternal = 0x4C;
// Window of APlayerController member slots swept for the PlayerCameraManager
// pointer. Starts past the UObject header and stops well beyond where UE4.27
// puts it, so the member is found without pinning an offset that a patch moves.
constexpr std::size_t kControllerMemberScanFirst = 0x28;
constexpr std::size_t kControllerMemberScanLast = 0xC00;

un::UObjectGlobalsLayout MakeLayout(std::uintptr_t gobjectsObjObjects,
                                    std::uintptr_t fnamePool) {
    un::UObjectGlobalsLayout L{};
    L.kObjObjects       = gobjectsObjObjects;  // already FUObjectArray + 0x10
    L.kObjObjects_Num   = kNumElementsInChunked;
    L.kFUObjectItemSize = kFUObjectItemSize;
    L.kChunkNumElems    = kChunkNumElems;
    L.kFNamePool        = fnamePool;
    L.kFNamePoolBlocks  = kFNamePoolBlocks;
    L.kClassPrivate     = kClassPrivate;
    L.kNamePrivate      = kNamePrivate;
    L.kOuterPrivate     = kOuterPrivate;
    return L;
}

bool ReadFloat(std::uintptr_t addr, float& out) {
    std::uint32_t bits = 0;
    if (!un::SafeReadU32(addr, bits)) return false;
    std::memcpy(&out, &bits, sizeof(out));
    return true;
}

// The core exposes 16/32/64-bit guarded reads but no 8-bit one, and this is the
// one place that needs it. See DecodeName.
bool SafeReadU8(std::uintptr_t addr, std::uint8_t& out) {
    __try {
        out = *reinterpret_cast<const std::uint8_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Decode an FName entry directly from a candidate pool (no global layout set).
//
// The ANSI branch reads a BYTE at a time. A uint16 read of the last character
// touches entry + 2 + len, one byte past the name, which faults when the name
// ends on the final byte of a committed page: the loop then breaks and the name
// comes back one character short. Here that means DecodeName(cand, 0) returns
// "Non" instead of "None", FNamePool discovery rejects the real pool, and the
// gate, the crosshair and the HUD compensation never initialise at all. It is
// ASLR-dependent, so it presents as head tracking intermittently not starting.
// cameraunlock-core's ResolveFName fixed exactly this; this copy had not caught
// up.
std::string DecodeName(std::uintptr_t poolBase, std::uint32_t id) {
    std::uintptr_t blockPtr = 0;
    const std::uintptr_t blocks = poolBase + kFNamePoolBlocks;
    if (!un::SafeReadPtr(blocks + (static_cast<std::uintptr_t>(id >> 16) * 8), blockPtr))
        return std::string();
    if (!un::LooksLikePointer(blockPtr)) return std::string();
    const std::uintptr_t entry = blockPtr + (static_cast<std::uintptr_t>(id & 0xffff) * 2);
    std::uint16_t header = 0;
    if (!un::SafeReadU16(entry, header)) return std::string();
    const bool wide = (header & 1) != 0;
    const int len = header >> 6;
    if (len <= 0 || len > 128) return std::string();
    std::string out;
    out.reserve(len);
    if (wide) {
        for (int i = 0; i < len; ++i) {
            std::uint16_t w = 0;
            if (!un::SafeReadU16(entry + 2 + i * 2, w)) break;
            out.push_back(static_cast<char>(w & 0x7f));
        }
    } else {
        for (int i = 0; i < len; ++i) {
            std::uint8_t b = 0;
            if (!SafeReadU8(entry + 2 + i, b)) break;
            out.push_back(static_cast<char>(b));
        }
    }
    return out;
}

bool IsSaneName(const std::string& s) {
    if (s.empty() || s.size() > 128) return false;
    for (char c : s) {
        unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u > 0x7e) return false;
    }
    return true;
}

// Iterate writable data sections of the module (.data / .bss), invoking
// fn(slotAddr) for each 8-byte-aligned slot. fn returns true to stop.
template <typename Fn>
void ForEachDataSlot(std::uintptr_t base, Fn&& fn) {
    // Every other read in this file is fault-guarded; these are raw. A repacked
    // or header-stripped EXE - the case the build-profile failsafe exists to
    // keep the mod dormant on - would fault here first, before the fingerprint
    // mismatch ever got a chance to say so.
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    if (dos->e_lfanew <= 0 || dos->e_lfanew >= 0x1000) return;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    const int n = nt->FileHeader.NumberOfSections;
    for (int i = 0; i < n; ++i) {
        const DWORD ch = sec[i].Characteristics;
        // Writable data: .data (initialized) and .bss (uninitialized). GUObjectArray
        // is zero-initialized and lives in .bss, so uninitialized data must be
        // included. Skip anything executable / code.
        if (!(ch & IMAGE_SCN_MEM_WRITE)) continue;
        if (ch & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE)) continue;
        if (!(ch & (IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_CNT_UNINITIALIZED_DATA)))
            continue;
        std::uintptr_t s = base + sec[i].VirtualAddress;
        std::uintptr_t e = s + sec[i].Misc.VirtualSize;
        s = (s + 7) & ~static_cast<std::uintptr_t>(7);
        for (std::uintptr_t p = s; p + 8 <= e; p += 8) {
            if (fn(p)) return;
        }
    }
}

// Find NamePoolData: the global whose Blocks[0] decodes id 0 to "None".
std::uintptr_t FindFNamePool(std::uintptr_t base) {
    std::uintptr_t found = 0;
    ForEachDataSlot(base, [&](std::uintptr_t cand) -> bool {
        // Quick reject: Blocks[0] must look like a heap pointer.
        std::uintptr_t block0 = 0;
        if (!un::SafeReadPtr(cand + kFNamePoolBlocks, block0)) return false;
        if (!un::LooksLikePointer(block0)) return false;
        if (DecodeName(cand, 0) == "None") {
            found = cand;
            return true;
        }
        return false;
    });
    return found;
}

// Find the FChunkedFixedUObjectArray (GUObjectArray.ObjObjects) directly, with
// no assumption about the FUObjectArray header size: scan for a slot laid out as
// {Objects(ptr), PreAllocated(ptr/0), MaxElements, NumElements, MaxChunks,
// NumChunks} whose first objects resolve to sane class names via the pool.
// Returns the address of the chunked array (what SetRuntime wants as kObjObjects).
std::uintptr_t FindObjObjects(std::uintptr_t base, std::uintptr_t pool, int& numOut,
                              bool verbose) {
    std::uintptr_t found = 0;
    int foundNum = 0;
    int numericHits = 0, bestSane = 0;
    std::uintptr_t bestCand = 0;
    int bestNum = 0;
    ForEachDataSlot(base, [&](std::uintptr_t cand) -> bool {
        std::uintptr_t chunks = 0;
        if (!un::SafeReadPtr(cand, chunks) || !un::LooksLikePointer(chunks))
            return false;
        std::uint32_t maxElems = 0, numElems = 0, maxChunks = 0, numChunks = 0;
        if (!un::SafeReadU32(cand + kMaxElementsInChunked, maxElems)) return false;
        if (!un::SafeReadU32(cand + kNumElementsInChunked, numElems)) return false;
        if (!un::SafeReadU32(cand + 0x18, maxChunks)) return false;
        if (!un::SafeReadU32(cand + 0x1C, numChunks)) return false;
        if (numElems < 1000 || numElems > 20000000) return false;
        if (numElems > maxElems || maxElems > 20000000) return false;
        if (numChunks == 0 || numChunks > maxChunks || maxChunks > 100000) return false;
        ++numericHits;

        // Resolve object class names through the pool. Early slots can be sparse,
        // so sample a wider window.
        int sane = 0;
        for (std::uint32_t i = 0; i < 64 && i < numElems; ++i) {
            std::uintptr_t chunk = 0;
            if (!un::SafeReadPtr(chunks + (static_cast<std::uintptr_t>(i / kChunkNumElems) * 8), chunk))
                break;
            if (!un::LooksLikePointer(chunk)) break;
            std::uintptr_t item = chunk + (i % kChunkNumElems) * kFUObjectItemSize;
            std::uintptr_t obj = 0;
            if (!un::SafeReadPtr(item, obj) || !un::LooksLikePointer(obj)) continue;
            std::uintptr_t cls = 0;
            if (!un::SafeReadPtr(obj + kClassPrivate, cls) || !un::LooksLikePointer(cls)) continue;
            std::uint32_t nameId = 0;
            if (!un::SafeReadU32(cls + kNamePrivate, nameId)) continue;
            if (IsSaneName(DecodeName(pool, nameId))) ++sane;
        }
        if (sane > bestSane) { bestSane = sane; bestCand = cand; bestNum = static_cast<int>(numElems); }
        if (sane >= 6) {
            found = cand;
            foundNum = static_cast<int>(numElems);
            return true;
        }
        return false;
    });
    if (!found && verbose) {
        log::Line("[ue4][diag] ObjObjects: %d numeric candidates, best sane=%d @ rva 0x%llX (num=%d)",
                  numericHits, bestSane,
                  static_cast<unsigned long long>(bestCand ? bestCand - base : 0), bestNum);
    }
    numOut = foundNum;
    return found;
}

}  // namespace

bool InitReflection(std::uintptr_t base, std::uintptr_t end, int& objCountOut) {
    objCountOut = 0;

    // Called on a retry loop during early engine init, so failures are silent:
    // the UObject array genuinely does not exist for the first second or so, and
    // reporting that as a problem meant every launch logged two alarming lines
    // and then succeeded. Only a failure that PERSISTS is worth a line, so the
    // first verbose attempt is deliberately well past the point where a healthy
    // startup has finished.
    static int s_attempts = 0;
    const int attempt = s_attempts++;
    const bool verbose = attempt >= 20 && (attempt % 40) == 0;

    std::uintptr_t pool = FindFNamePool(base);
    if (!pool) {
        if (verbose) log::Line("[ue4][diag] FNamePool not found yet.");
        return false;
    }

    int num = 0;
    std::uintptr_t objObjects = FindObjObjects(base, pool, num, verbose);
    if (!objObjects) {
        if (verbose)
            log::Line("[ue4][diag] FNamePool ok (rva=0x%llX) but ObjObjects not found yet.",
                      static_cast<unsigned long long>(pool - base));
        return false;
    }

    // cameraunlock::unreal helpers index off ModuleBase, so they take RVAs.
    // Published before validation because the validation itself goes through
    // those helpers - which is why the failure path below has to RETRACT it.
    // Leaving a rejected candidate installed means ModuleBase() is non-zero and
    // every reflection call in the process will happily walk it; the only reason
    // that is not live today is that each caller also consults
    // CameraHook::m_reflectionReady, and that is one new caller away from being
    // a scan that hands Reticle::Write arbitrary addresses to write into.
    un::SetRuntime(base, end, MakeLayout(objObjects - base, pool - base));

    // Final validation through the core helpers.
    int total = 0, sane = 0;
    bool sawCore = false;
    un::ForEachUObject([&](std::uintptr_t obj) -> bool {
        ++total;
        std::string cls = un::ClassName(obj);
        if (IsSaneName(cls)) {
            ++sane;
            if (cls == "Class" || cls == "Package" || cls == "Function") sawCore = true;
        }
        return total >= 3000;
    });
    if (total < 500 || !sawCore || (static_cast<double>(sane) / total) < 0.6) {
        // Retract, so a rejected candidate is not left installed as the global
        // layout for anything else that touches reflection.
        un::SetRuntime(0, 0, un::UObjectGlobalsLayout{});
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            log::Line("[ue4][diag] ObjObjects candidate @ rva 0x%llX (num=%d) but core "
                      "validation failed: total=%d sane=%d core=%d",
                      static_cast<unsigned long long>(objObjects - base), num,
                      total, sane, sawCore);
        }
        return false;
    }

    objCountOut = num;
    log::Line("[ue4] reflection ready: FNamePool rva=0x%llX ObjObjects rva=0x%llX NumElements=%d",
              static_cast<unsigned long long>(pool - base),
              static_cast<unsigned long long>(objObjects - base), num);
    return true;
}

std::uintptr_t FindCameraManager() {
    std::uintptr_t found = 0;
    un::ForEachUObject([&](std::uintptr_t obj) -> bool {
        std::string cls = un::ClassName(obj);
        if (!un::ContainsCI(cls, "PlayerCameraManager")) return false;
        std::string name = un::ObjectName(obj);
        if (name.rfind("Default__", 0) == 0) return false;  // skip CDOs
        if (!IsObjectAlive(obj)) return false;
        found = obj;
        return true;
    });
    return found;
}

std::uintptr_t FindLiveInstance(const char* classNameFragment) {
    if (!classNameFragment) return 0;
    std::uintptr_t found = 0;
    un::ForEachUObject([&](std::uintptr_t obj) -> bool {
        if (!un::ContainsCI(un::ClassName(obj), classNameFragment)) return false;
        if (un::ObjectName(obj).rfind("Default__", 0) == 0) return false;
        if (!IsObjectAlive(obj)) return false;
        found = obj;
        return true;
    });
    return found;
}

std::uintptr_t FindClassByName(const char* className) {
    if (!className) return 0;
    std::uintptr_t found = 0;
    un::ForEachUObject([&](std::uintptr_t obj) -> bool {
        if (un::ObjectName(obj) != className) return false;
        // Structs and enums can share a name with a class, so require the object
        // to actually BE a class. Not "== Class" though: a Blueprint's class is a
        // WidgetBlueprintGeneratedClass or BlueprintGeneratedClass, and an exact
        // match silently resolved every Blueprint class to nothing.
        const std::string metaClass = un::ClassName(obj);
        if (metaClass.size() < 5
            || metaClass.compare(metaClass.size() - 5, 5, "Class") != 0) {
            return false;
        }
        found = obj;
        return true;
    });
    return found;
}

bool FindPropertyOffset(std::uintptr_t classObject, const char* propertyName,
                        std::size_t& offsetOut) {
    if (!classObject || !propertyName) return false;
    std::uintptr_t cls = classObject;
    for (int hops = 0; cls && hops < 32; ++hops) {
        std::uintptr_t field = 0;
        if (un::SafeReadPtr(cls + kChildProperties, field)) {
            for (int i = 0; field && i < 8192; ++i) {
                std::uint32_t nameId = 0;
                std::uint32_t offset = 0;
                if (un::SafeReadU32(field + kFieldName, nameId)
                    && un::ResolveFName(nameId) == propertyName
                    && un::SafeReadU32(field + kPropertyOffsetInternal, offset)) {
                    offsetOut = offset;
                    return true;
                }
                std::uintptr_t next = 0;
                if (!un::SafeReadPtr(field + kFieldNext, next)) break;
                field = next;
            }
        }
        std::uintptr_t super = 0;
        if (!un::SafeReadPtr(cls + kSuperStruct, super)) break;
        cls = super;
    }
    return false;
}

std::uintptr_t ClassOf(std::uintptr_t obj) {
    std::uintptr_t cls = 0;
    if (!obj || !un::SafeReadPtr(obj + kClassPrivate, cls)) return 0;
    return cls;
}

// Only objects the game CREATED at runtime are on screen. RF_WasLoaded is what
// separates those from anything that came off disk, and it is the flag that
// matters: a Blueprint's widget-tree templates are NOT archetype-flagged (checked
// against the running game - they read RF_Transactional|RF_WasLoaded|
// RF_LoadCompleted = 0x00280008, against 0x00000008 for a live instance), so
// filtering on CDO and archetype alone lets every template through.
static bool IsRuntimeCreated(std::uintptr_t obj) {
    std::uint32_t flags = 0;
    if (!un::SafeReadU32(obj + kObjectFlags, flags)) return false;
    return (flags & (kFlagClassDefaultObject | kFlagArchetypeObject | kFlagWasLoaded)) == 0;
}

int FindInstancesOfClass(std::uintptr_t classObject, std::uintptr_t* out, int maxInstances) {
    if (!classObject || !out || maxInstances <= 0) return 0;
    int n = 0;
    un::ForEachUObject([&](std::uintptr_t obj) -> bool {
        // One read per object. Matching on the class NAME instead would resolve
        // an FName and allocate a string for every object in the process, which
        // at a few hundred thousand objects is a visible hitch on a timer.
        std::uintptr_t cls = 0;
        if (!un::SafeReadPtr(obj + kClassPrivate, cls) || cls != classObject) return false;
        if (!IsRuntimeCreated(obj)) return false;
        out[n++] = obj;
        return n >= maxInstances;
    });
    return n;
}

bool IsObjectAlive(std::uintptr_t obj) {
    if (!obj) return false;
    std::uint32_t index = 0;
    if (!un::SafeReadU32(obj + kInternalIndex, index)) return false;

    const auto& layout = un::Layout();
    // Zero before SetRuntime has run, and `index / 0` is a hardware #DE outside
    // every fault guard - an instant crash rather than a failed read. The core's
    // ForEachUObject guards the same division for the same reason.
    if (!layout.kChunkNumElems || !layout.kFUObjectItemSize) return false;
    const std::uintptr_t array = un::ModuleBase() + layout.kObjObjects;
    std::uintptr_t chunks = 0;
    std::uint32_t count = 0;
    if (!un::SafeReadPtr(array, chunks) || !chunks) return false;
    if (!un::SafeReadU32(array + layout.kObjObjects_Num, count) || index >= count) return false;

    std::uintptr_t chunk = 0;
    if (!un::SafeReadPtr(chunks + (index / layout.kChunkNumElems) * sizeof(void*), chunk)
        || !chunk) {
        return false;
    }
    const std::uintptr_t item =
        chunk + (index % layout.kChunkNumElems) * layout.kFUObjectItemSize;

    // The slot has to still hold this exact object: once it is recycled the
    // index belongs to something else entirely.
    std::uintptr_t stored = 0;
    if (!un::SafeReadPtr(item, stored) || stored != obj) return false;
    std::uint32_t itemFlags = 0;
    if (!un::SafeReadU32(item + kFUObjectItemFlags, itemFlags)) return false;
    return (itemFlags & (kInternalUnreachable | kInternalPendingKill)) == 0;
}

// First-by-index, so a level transition's outgoing controller - Destroy()d,
// PendingKill, but still in the object array until the next GC, and at a LOWER
// index than the one just spawned - would win every scan for tens of seconds.
// Its Pawn is null because Destroy() unpossesses, so the gate would report "no
// possessed pawn" for the whole of the new level while rescanning the entire
// object array once a second to find the same corpse. Hence the liveness test.
std::uintptr_t FindPlayerController() {
    std::uintptr_t found = 0;
    un::ForEachUObject([&](std::uintptr_t obj) -> bool {
        std::string cls = un::ClassName(obj);
        if (!un::ContainsCI(cls, "PlayerController")) return false;
        std::string name = un::ObjectName(obj);
        if (name.rfind("Default__", 0) == 0) return false;  // skip CDOs
        if (!IsObjectAlive(obj)) return false;
        found = obj;
        return true;
    });
    return found;
}

// The class-name scan can grab a stale PlayerCameraManager from a streamed-out
// level. The one actually rendering is the local PlayerController's
// PlayerCameraManager member. We don't hardcode its offset: scan the controller
// instance for a pointer to a PlayerCameraManager-class object. Returns the
// camera manager (and reports the controller + member offset), or 0.
std::uintptr_t FindActiveCameraManager(std::uintptr_t& pcOut, std::size_t& offOut) {
    pcOut = 0;
    offOut = 0;
    std::uintptr_t pc = FindPlayerController();
    if (!pc) return 0;
    pcOut = pc;
    for (std::size_t off = kControllerMemberScanFirst; off <= kControllerMemberScanLast;
         off += sizeof(void*)) {
        std::uintptr_t p = 0;
        if (!un::SafeReadPtr(pc + off, p) || !un::LooksLikePointer(p)) continue;
        if (!un::ContainsCI(un::ClassName(p), "PlayerCameraManager")) continue;
        // The sweep takes the FIRST pointer-shaped slot whose class matches, and
        // a stale TWeakObjectPtr payload or a previous level's manager can sit at
        // a lower offset than the real member. A destroyed manager keeps its
        // ClassPrivate indefinitely, so the caller's class-pointer revalidation
        // would never notice and the heartbeat would report a frozen clean POV -
        // which reads as proof the decoupling works.
        if (!IsObjectAlive(p)) continue;
        offOut = off;
        return p;
    }
    return 0;
}

bool ReadPovRotation(std::uintptr_t cm, std::size_t rotOffset, FRotator& out) {
    return ReadFloat(cm + rotOffset + 0, out.Pitch)
        && ReadFloat(cm + rotOffset + 4, out.Yaw)
        && ReadFloat(cm + rotOffset + 8, out.Roll);
}

bool ReadPovFov(std::uintptr_t cm, std::size_t rotOffset, float& out) {
    return ReadFloat(cm + rotOffset + 12, out);
}

}  // namespace pdht::ue4
