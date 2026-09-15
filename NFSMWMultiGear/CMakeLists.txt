cmake_minimum_required(VERSION 3.16)

project(NFSMWMultiGear VERSION 1.0.0 LANGUAGES C CXX RC)

# The game is a 32-bit PE.  Keep this check here as well as in the SDK so a
# standalone configure cannot accidentally produce a DLL the ASI loader cannot
# load.
if(NOT WIN32)
    message(FATAL_ERROR "NFSMWMultiGear must be built on Windows or with a Win32 toolchain")
endif()
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "NFSMWMultiGear requires the Win32/x86 generator")
endif()
if(NOT MSVC)
    message(FATAL_ERROR "NFSMWMultiGear Phase 4 requires MSVC for guarded memory access")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(NFSMW_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../nfsmw-2005-sdk" CACHE PATH
    "Path to the nfsmw-2005-sdk checkout")
set(NFSMW_BUILD_EXAMPLES OFF CACHE BOOL "Do not build the SDK examples")
set(NFSMW_HOOKS_BACKEND minhook CACHE STRING "Use the SDK's relocatable hook backend")
add_subdirectory("${NFSMW_SDK_DIR}" "nfsmw-sdk-build" EXCLUDE_FROM_ALL)

# `nfsmw_add_plugin` is defined by the SDK helper but invoked from this
# directory.  Re-state its path inputs in the caller scope because CMake
# directory variables are not implicitly exported across add_subdirectory.
set(NFSMW_SDK_INCLUDE_DIR "${NFSMW_SDK_DIR}/include")
# The project-local shim accepts either the original speed.exe name or the
# branded Reforged name, then runs the startup gate before installing hooks.
set(NFSMW_SDK_ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/ReforgedEntry.c")
set(NFSMW_SDK_MINHOOK_DIR "${NFSMW_SDK_DIR}/extern/minhook")
set(NFSMW__MINHOOK_SRCS
    "${NFSMW_SDK_MINHOOK_DIR}/src/buffer.c"
    "${NFSMW_SDK_MINHOOK_DIR}/src/hook.c"
    "${NFSMW_SDK_MINHOOK_DIR}/src/trampoline.c"
    "${NFSMW_SDK_MINHOOK_DIR}/src/hde/hde32.c"
    "${NFSMW_SDK_MINHOOK_DIR}/src/hde/hde64.c")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/NFSMWMultiGear.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/NFSMWMultiGear.rc"
    @ONLY)
nfsmw_add_plugin(NFSMWMultiGear
    SOURCES
        "${CMAKE_CURRENT_BINARY_DIR}/NFSMWMultiGear.rc"
        src/Phase1.cpp
        src/GameAccess.cpp
        src/DiagnosticLog.cpp
        src/HudGearDisplay.cpp
        src/StartupGate.cpp
        GearConfig.cpp)

# nfsmw_add_plugin() supplies the SDK include and MinHook paths.  These target
# properties keep the resulting ASI self-contained for end users.
set_property(TARGET NFSMWMultiGear PROPERTY MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_property(TARGET NFSMWMultiGear APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/NFSMWMultiGear.cfg"
    "${CMAKE_CURRENT_SOURCE_DIR}/README.zh-CN.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/TECHNICAL_NOTES.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/BUILDING.md")
target_link_libraries(NFSMWMultiGear PRIVATE advapi32 crypt32)
target_compile_definitions(NFSMWMultiGear PRIVATE
    NFSMW_MULTIGEAR_VERSION="${PROJECT_VERSION}"
    NFSMW_MULTIGEAR_DISABLE_LOG=1
    # Install the observer as soon as the ASI worker can run, so constructor
    # events are not discarded by the SDK's general-purpose two-second delay.
    NFSMW_ASI_INIT_DELAY_MS=0)

if(MSVC)
    target_compile_options(NFSMWMultiGear PRIVATE
        /utf-8
        $<$<COMPILE_LANGUAGE:CXX>:/W4>
        $<$<COMPILE_LANGUAGE:C>:/W0>)
endif()

# Keep the build output directly usable as the ASI-only release package.
# The SDK's .dll/BepInEx identity is intentionally not distributed.
add_custom_command(TARGET NFSMWMultiGear POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/NFSMWMultiGear.cfg"
        "$<TARGET_FILE_DIR:NFSMWMultiGear>/NFSMWMultiGear.cfg"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/README.zh-CN.md"
        "$<TARGET_FILE_DIR:NFSMWMultiGear>/README.zh-CN.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/TECHNICAL_NOTES.md"
        "$<TARGET_FILE_DIR:NFSMWMultiGear>/TECHNICAL_NOTES.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/BUILDING.md"
        "$<TARGET_FILE_DIR:NFSMWMultiGear>/BUILDING.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${NFSMW_SDK_DIR}/LICENSE"
        "$<TARGET_FILE_DIR:NFSMWMultiGear>/LICENSE.nfsmw-sdk.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${NFSMW_SDK_MINHOOK_DIR}/LICENSE.txt"
        "$<TARGET_FILE_DIR:NFSMWMultiGear>/LICENSE.MinHook.txt")

install(FILES "$<TARGET_FILE:NFSMWMultiGear>"
    DESTINATION "." RENAME "NFSMWMultiGear.asi" COMPONENT Runtime)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/NFSMWMultiGear.cfg"
    "${CMAKE_CURRENT_SOURCE_DIR}/README.zh-CN.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/TECHNICAL_NOTES.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/BUILDING.md"
    DESTINATION "." COMPONENT Runtime)
install(FILES "${NFSMW_SDK_DIR}/LICENSE"
    DESTINATION "." RENAME "LICENSE.nfsmw-sdk.txt" COMPONENT Runtime)
install(FILES "${NFSMW_SDK_MINHOOK_DIR}/LICENSE.txt"
    DESTINATION "." RENAME "LICENSE.MinHook.txt" COMPONENT Runtime)

include(CTest)
if(BUILD_TESTING)
    add_executable(GearConfigTests
        tests/GearConfigTests.cpp
        GearConfig.cpp)
    set_property(TARGET GearConfigTests PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    if(MSVC)
        target_compile_options(GearConfigTests PRIVATE /utf-8 /W4 /WX)
    endif()
    add_test(NAME GearConfigTests
        COMMAND GearConfigTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/data")

    add_executable(GearConfigPhase4Tests
        tests/GearConfigPhase4Tests.cpp
        GearConfig.cpp)
    set_property(TARGET GearConfigPhase4Tests PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    if(MSVC)
        target_compile_options(GearConfigPhase4Tests PRIVATE /utf-8 /W4 /WX)
    endif()
    add_test(NAME GearConfigPhase4Tests
        COMMAND GearConfigPhase4Tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/data")

    add_executable(GameAccessTests
        tests/GameAccessTests.cpp
        src/GameAccess.cpp)
    set_property(TARGET GameAccessTests PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_libraries(GameAccessTests PRIVATE advapi32 crypt32)
    if(MSVC)
        target_compile_options(GameAccessTests PRIVATE /utf-8 /W4 /WX)
    endif()
    add_test(NAME GameAccessTests COMMAND GameAccessTests)

    add_executable(HudGearDisplayTests
        tests/HudGearDisplayTests.cpp
        src/HudGearDisplay.cpp
        src/GameAccess.cpp)
    set_property(TARGET HudGearDisplayTests PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_libraries(HudGearDisplayTests PRIVATE advapi32 crypt32)
    if(MSVC)
        target_compile_options(HudGearDisplayTests PRIVATE /utf-8 /W4 /WX)
    endif()
    add_test(NAME HudGearDisplayTests COMMAND HudGearDisplayTests)

    add_executable(StartupGateTests
        tests/StartupGateTests.cpp
        src/StartupGate.cpp)
    set_property(TARGET StartupGateTests PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_libraries(StartupGateTests PRIVATE advapi32 crypt32)
    if(MSVC)
        target_compile_options(StartupGateTests PRIVATE /utf-8 /W4 /WX)
    endif()
    add_test(NAME StartupGateTests COMMAND StartupGateTests)

    add_executable(DiagnosticLogTests
        tests/DiagnosticLogTests.cpp
        src/DiagnosticLog.cpp)
    set_property(TARGET DiagnosticLogTests PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_definitions(DiagnosticLogTests PRIVATE
        NFSMW_MULTIGEAR_DISABLE_LOG=1)
    if(MSVC)
        target_compile_options(DiagnosticLogTests PRIVATE /utf-8 /W4 /WX)
    endif()
    add_test(NAME DiagnosticLogTests COMMAND DiagnosticLogTests)

    set(NFSMW_REFORGED_TEST_EXE
        "${CMAKE_CURRENT_SOURCE_DIR}/outputs/Need For Speed MW-Reforged.exe"
        CACHE FILEPATH "Signed Reforged executable for startup-gate tests")
    if(NFSMW_REFORGED_TEST_EXE)
        if(NOT EXISTS "${NFSMW_REFORGED_TEST_EXE}")
            message(FATAL_ERROR
                "NFSMW_REFORGED_TEST_EXE does not exist: ${NFSMW_REFORGED_TEST_EXE}")
        endif()
        add_test(NAME StartupGateStampedExecutableTests
            COMMAND StartupGateTests "${NFSMW_REFORGED_TEST_EXE}")
    endif()
endif()
