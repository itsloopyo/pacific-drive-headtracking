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
    src/legacy_config/legacy_config.cpp
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

    # The canonical config, the committed file and the toggles' saves. Also the
    # renderer behind `pixi run render-config` (--render-config <path>).
    add_executable(pdht_config_tests
        tests/config_tests.cpp
        src/config.cpp
        src/legacy_config/legacy_config.cpp
    )
    target_include_directories(pdht_config_tests PRIVATE src)
    target_link_libraries(pdht_config_tests PRIVATE cameraunlock)
    target_compile_definitions(pdht_config_tests PRIVATE PDHT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_options(pdht_config_tests PRIVATE
        /W4 /EHsc
        $<$<CONFIG:Release>:/O2>
    )

    set(PDHT_DIFFERENTIAL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/config_differential)

    # Boundary tests for the frozen PacificDriveHeadTracking.ini reader, which
    # imports a player's pre-canonical file.
    add_executable(pdht_config_validation
        ${PDHT_DIFFERENTIAL_DIR}/legacy_reader_tests.cpp
        src/legacy_config/legacy_config.cpp
    )
    target_include_directories(pdht_config_validation PRIVATE src)
    target_link_libraries(pdht_config_validation PRIVATE cameraunlock)
    target_compile_options(pdht_config_validation PRIVATE
        /W4 /EHsc
        $<$<CONFIG:Release>:/O2>
    )

    # The canonical config conversion's differential test: the dev
    # pre-release's reader (byte copies in tests/config_differential/oracle/src)
    # and startup code against the frozen reader in src/legacy_config/, and that
    # reader against the config owner's migration into CameraUnlock.ini.
    add_executable(pdht_config_differential
        ${PDHT_DIFFERENTIAL_DIR}/differential_tests.cpp
        ${PDHT_DIFFERENTIAL_DIR}/oracle/oracle_reader.cpp
        src/config.cpp
        src/legacy_config/legacy_config.cpp
    )
    target_include_directories(pdht_config_differential PRIVATE
        src
        ${PDHT_DIFFERENTIAL_DIR}
    )
    target_compile_definitions(pdht_config_differential PRIVATE
        PDHT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(pdht_config_differential PRIVATE cameraunlock)
    target_compile_options(pdht_config_differential PRIVATE
        /W4 /EHsc
        $<$<CONFIG:Release>:/O2>
    )

    # What the differential test compiles is pinned by hash, line endings aside:
    # the oracle's byte copies must stay the dev build's (git show
    # 21a4f1e:src/<file>), the core sources both readers compile must stay the
    # ones that build was compiled with (core 7820a8c), and the frozen reader is
    # never edited.
    function(pdht_require_hash path expected)
        file(READ "${path}" text)
        string(REPLACE "\r" "" text "${text}")
        string(SHA256 actual "${text}")
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "${path} has changed (SHA-256 of its text ${actual}, pinned "
                                "${expected}). The config differential test compares against "
                                "exactly this file; see CMakeLists.txt.")
        endif()
    endfunction()
    set(PDHT_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/cameraunlock-core/cpp)
    foreach(pinned
            "${PDHT_DIFFERENTIAL_DIR}/oracle/src/config.cpp|1077bceeeac7c6029510b9a9bd65908b470b4a3ac83496bb4dc99e95627fbf88"
            "${PDHT_DIFFERENTIAL_DIR}/oracle/src/config.h|1931d73c0e886508874e397fbc08f213c7b26f807bcfe8a1253c23e8267af3dd"
            "${PDHT_CORE_DIR}/include/cameraunlock/config/ini_reader.h|a7ffb44210ff59672fa97e8e5feaa2cb3e81938fcc0334a384c68bc371b3857a"
            "${PDHT_CORE_DIR}/src/config/ini_reader.cpp|e01515c2656aaf533bae4350dc45b702c3e3d4043935743dcc9bd5ea581fbe1c"
            "${PDHT_CORE_DIR}/include/cameraunlock/math/finite_utils.h|c59772d698d54ade3374ee1221b74f5563a86d76f0eb34a989ef7efab389c0ad"
            "${PDHT_CORE_DIR}/include/cameraunlock/logging/file_log.h|43bdd2ef8554c78e5f440333463750c13b95110fe672b0b6e273244df9e7d169"
            "${PDHT_CORE_DIR}/src/logging/file_log.cpp|73c53c2baa06bbfebe8211f62678aa2b60cb95f604743d3686951ba56b87ea47"
            "${PDHT_CORE_DIR}/include/cameraunlock/data/position_settings.h|b24dceb8e25475aebc5a468a5c7362a4a4e64204d183d1408525345f32f547f5"
            "${PDHT_CORE_DIR}/include/cameraunlock/math/smoothing_utils.h|fc2146f8c585e5f610c7234e302f59de4945679cfa28ff479ca47477ec073f22"
            "${PDHT_CORE_DIR}/include/cameraunlock/math/angle_utils.h|d7a905270933e3cb0c4c361d29d3fd701655498cbcd1875ea79d180468bdbe6a"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/legacy_config/legacy_config.h|f6e4e0ab3f0546dfb4ca40d62de2e8173353190bc51e4f7e9ad2846fb64ff39e"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/legacy_config/legacy_config.cpp|d2069b8beb6734c61463fcc3e724ba8246bcae2a78c9a0bb05b1318f113082c2")
        string(REPLACE "|" ";" pair "${pinned}")
        list(GET pair 0 pinned_path)
        list(GET pair 1 pinned_hash)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${pinned_path}")
        pdht_require_hash("${pinned_path}" "${pinned_hash}")
    endforeach()

    enable_testing()
    add_test(NAME view_characterization COMMAND pdht_view_characterization)
    add_test(NAME config_validation COMMAND pdht_config_validation)
    add_test(NAME config_tests COMMAND pdht_config_tests)
    add_test(NAME config_differential COMMAND pdht_config_differential)
    # Core's canonical config lint over the committed file and every file the
    # differential test migrated, which it writes beside itself under migrated\.
    add_test(NAME config_lint COMMAND node
        ${PDHT_DIFFERENTIAL_DIR}/lint-migrated.mjs
        $<TARGET_FILE_DIR:pdht_config_differential>/migrated)
    set_tests_properties(config_lint PROPERTIES DEPENDS config_differential)
    set_tests_properties(config_differential config_lint PROPERTIES LABELS differential)
endif()
