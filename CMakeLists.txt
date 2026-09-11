cmake_minimum_required(VERSION 3.20)
project(PacificDriveHeadTracking VERSION 0.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT MSVC)
    message(FATAL_ERROR "This mod targets the MSVC toolchain (x64).")
endif()

add_definitions(-DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)

# MinHook: the only build-time external. CMake target name is `minhook`, which
# is exactly what cameraunlock-core's CAMERAUNLOCK_BUILD_HOOKS option expects.
#
# Pinned to the commit SHA, not the tag. A git tag is a mutable ref: whoever
# controls the upstream repository can retarget v1.3.4 at any time, and every
# clean configure here - a contributor's and CI's alike - would fetch and link
# it into a DLL that gets injected into the player's game process, with nothing
# in the diff to show for it. The trailing comment records which release the SHA
# is, so a bump stays readable.
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        c3fcafdc10146beb5919319d0683e44e3c30d537  # v1.3.4
)
FetchContent_MakeAvailable(minhook)

# Pull in cameraunlock-core's C++ static lib + hook manager. Tests off (we don't
# build its gtest suite); hooks on (needs the `minhook` target above).
set(CAMERAUNLOCK_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(CAMERAUNLOCK_BUILD_HOOKS   ON  CACHE BOOL "" FORCE)
set(CAMERAUNLOCK_BUILD_UNREAL  ON  CACHE BOOL "" FORCE)
add_subdirectory(cameraunlock-core/cpp)

add_library(PacificDriveHeadTracking SHARED
    src/dllmain.cpp
    src/mod.cpp
    src/config.cpp
    src/camera_hook.cpp
    src/detour.cpp
    src/game_state.cpp
    src/marker_hook.cpp
    src/reticle.cpp
    src/ue4.cpp
    src/ue4_reflect.cpp
    src/steam_offsets.cpp
    src/gdk_offsets.cpp
)

target_include_directories(PacificDriveHeadTracking PRIVATE src)

target_link_libraries(PacificDriveHeadTracking PRIVATE
    cameraunlock
    cameraunlock_hooks
    cameraunlock_unreal
    minhook
    ws2_32
    d3d11
    dxgi
)

# Ship as a .asi plugin loaded by the Ultimate ASI Loader.
set_target_properties(PacificDriveHeadTracking PROPERTIES
    PREFIX ""
    SUFFIX ".asi"
    OUTPUT_NAME "PacificDriveHeadTracking"
)

# __try/__except works under /EHsc - /EHa only changes whether a C++ catch(...)
# also catches SEH, and nothing here does that. A per-source /EHa override was
# carried for src/ue4_reflect.cpp on the strength of a comment that was simply
# wrong about which files use SEH, and MSVC warned D9025 for it on every build.
# /guard:cf costs nothing and this DLL runs inside someone else's process.
target_compile_options(PacificDriveHeadTracking PRIVATE
    /W4 /EHsc /guard:cf
    $<$<CONFIG:Release>:/O2>
)

# Characterization harness for the view math. OFF by default so `pixi run build`
# and CI keep building exactly the shipped target and nothing else; turn it on
# with -DPDHT_BUILD_TESTS=ON before restructuring anything in ue4.cpp.
option(PDHT_BUILD_TESTS "Build the ue4 view characterization harness" OFF)
if(PDHT_BUILD_TESTS)
    add_executable(pdht_view_characterization
        tests/ue4_view_characterization.cpp
        src/ue4.cpp
        src/steam_offsets.cpp
        src/gdk_offsets.cpp
    )
    target_include_directories(pdht_view_characterization PRIVATE src)
    target_link_libraries(pdht_view_characterization PRIVATE cameraunlock)
    target_compile_options(pdht_view_characterization PRIVATE
        /W4 /EHsc
        $<$<CONFIG:Release>:/O2>
    )
    # Boundary tests for the INI, the one place the mod takes raw user input.
    add_executable(pdht_config_validation
        tests/config_validation.cpp
        src/config.cpp
    )
    target_include_directories(pdht_config_validation PRIVATE src)
    target_link_libraries(pdht_config_validation PRIVATE cameraunlock)
    target_compile_options(pdht_config_validation PRIVATE
        /W4 /EHsc
        $<$<CONFIG:Release>:/O2>
    )

    enable_testing()
    add_test(NAME view_characterization COMMAND pdht_view_characterization)
    add_test(NAME config_validation COMMAND pdht_config_validation)
endif()
