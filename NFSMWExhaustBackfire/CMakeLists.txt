cmake_minimum_required(VERSION 3.16)

project(NFSMWExhaustBackfire VERSION 1.1.15 LANGUAGES C CXX)

if(MSVC)
    # The ASI is injected into a 2005 process; keep the package independent
    # from a separately installed Visual C++ runtime and use one CRT model for
    # every static library linked into it.
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(NFSMW_EXHAUST_BUILD_C_API
       "Build the portable callback API used by the native game adapter" ON)
option(NFSMW_EXHAUST_ENABLE_LOGGING
       "Create the diagnostic log and emit debugger diagnostics" OFF)

set(_native_adapter_default OFF)
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(_native_adapter_default ON)
endif()
option(NFSMW_EXHAUST_BUILD_NATIVE_ADAPTER
       "Build the target-specific Win32/x86 ASI" ${_native_adapter_default})

add_library(nfsmw_exhaust_core STATIC
    src/AudioBank.cpp
    src/Config.cpp
    src/ConfigFile.cpp
    src/ExhaustController.cpp
)

target_include_directories(nfsmw_exhaust_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(nfsmw_exhaust_core PUBLIC cxx_std_17)

if(NFSMW_EXHAUST_BUILD_C_API)
    add_library(nfsmw_exhaust_api STATIC
        src/PluginApi.cpp
    )
    target_link_libraries(nfsmw_exhaust_api PUBLIC nfsmw_exhaust_core)
    target_include_directories(nfsmw_exhaust_api
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_features(nfsmw_exhaust_api PUBLIC cxx_std_17)
endif()

if(NFSMW_EXHAUST_BUILD_NATIVE_ADAPTER)
    if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
        message(FATAL_ERROR "The native NFSMW adapter requires a Win32/x86 generator")
    endif()
    if(NOT MSVC)
        message(FATAL_ERROR "The native adapter requires MSVC guarded calls")
    endif()
    if(NOT TARGET nfsmw_exhaust_api)
        message(FATAL_ERROR "The native adapter requires NFSMW_EXHAUST_BUILD_C_API=ON")
    endif()

    set(NFSMW_MINHOOK_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/../nfsmw-2005-sdk/extern/minhook"
        CACHE PATH "Path to the MinHook source directory")
    if(NOT EXISTS "${NFSMW_MINHOOK_DIR}/include/MinHook.h")
        message(FATAL_ERROR
            "MinHook was not found. Set NFSMW_MINHOOK_DIR to its source directory")
    endif()

    add_library(nfsmw_exhaust_native SHARED
        src/NativeAudio.cpp
        src/NativeAdapter.cpp
        src/NativeEntry.c
        src/NativeLog.cpp
        src/StartupGate.cpp
        src/SunSetLighting.cpp
        "${NFSMW_MINHOOK_DIR}/src/buffer.c"
        "${NFSMW_MINHOOK_DIR}/src/hook.c"
        "${NFSMW_MINHOOK_DIR}/src/trampoline.c"
        "${NFSMW_MINHOOK_DIR}/src/hde/hde32.c"
        "${NFSMW_MINHOOK_DIR}/src/hde/hde64.c"
    )
    target_include_directories(nfsmw_exhaust_native PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${NFSMW_MINHOOK_DIR}/include
        ${NFSMW_MINHOOK_DIR}/src
        ${NFSMW_MINHOOK_DIR}/src/hde
    )
    target_link_libraries(nfsmw_exhaust_native PRIVATE
        nfsmw_exhaust_api
        advapi32
        crypt32
        ole32
        xaudio2
    )
    target_compile_features(nfsmw_exhaust_native PRIVATE cxx_std_17)
    target_compile_definitions(nfsmw_exhaust_native PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
        NFSMW_EXHAUST_ENABLE_LOGGING=$<BOOL:${NFSMW_EXHAUST_ENABLE_LOGGING}>
    )
    set_target_properties(nfsmw_exhaust_native PROPERTIES
        OUTPUT_NAME "NFSMWExhaustBackfire"
        PREFIX ""
        SUFFIX ".asi"
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

if(MSVC)
    target_compile_options(nfsmw_exhaust_core PRIVATE /W4 /permissive-)
    if(TARGET nfsmw_exhaust_api)
        target_compile_options(nfsmw_exhaust_api PRIVATE /W4 /permissive- /EHsc)
    endif()
    if(TARGET nfsmw_exhaust_native)
        target_compile_options(nfsmw_exhaust_native PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:/W4>
            $<$<COMPILE_LANGUAGE:CXX>:/EHsc>
            /utf-8)
    endif()
else()
    target_compile_options(nfsmw_exhaust_core PRIVATE -Wall -Wextra -Wpedantic)
    if(TARGET nfsmw_exhaust_api)
        target_compile_options(nfsmw_exhaust_api PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(nfsmw_exhaust_nitrous_activity_tests
        tests/NitrousActivityTests.cpp
    )
    target_include_directories(nfsmw_exhaust_nitrous_activity_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(nfsmw_exhaust_nitrous_activity_tests PRIVATE
        cxx_std_17)
    if(MSVC)
        target_compile_options(nfsmw_exhaust_nitrous_activity_tests PRIVATE
            /W4 /EHsc)
    endif()
    add_test(NAME nfsmw_exhaust_nitrous_activity
             COMMAND nfsmw_exhaust_nitrous_activity_tests)

    add_executable(nfsmw_exhaust_tests
        tests/ExhaustControllerTests.cpp
    )
    target_link_libraries(nfsmw_exhaust_tests PRIVATE nfsmw_exhaust_core)
    target_compile_features(nfsmw_exhaust_tests PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(nfsmw_exhaust_tests PRIVATE /EHsc)
    endif()
    add_test(NAME nfsmw_exhaust_controller
             COMMAND nfsmw_exhaust_tests "${CMAKE_CURRENT_SOURCE_DIR}")

    if(TARGET nfsmw_exhaust_api)
        add_executable(nfsmw_exhaust_api_tests
            tests/PluginApiTests.cpp
        )
        target_link_libraries(nfsmw_exhaust_api_tests
            PRIVATE nfsmw_exhaust_api)
        target_compile_features(nfsmw_exhaust_api_tests PRIVATE cxx_std_17)
        if(MSVC)
            target_compile_options(nfsmw_exhaust_api_tests PRIVATE /EHsc)
        endif()
        add_test(NAME nfsmw_exhaust_plugin_api
                 COMMAND nfsmw_exhaust_api_tests
                         "${CMAKE_CURRENT_SOURCE_DIR}")

        add_executable(nfsmw_exhaust_c_header_test
            tests/PluginApiCHeaderTest.c
        )
        target_include_directories(nfsmw_exhaust_c_header_test
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_compile_features(nfsmw_exhaust_c_header_test PRIVATE c_std_99)
        add_test(NAME nfsmw_exhaust_c_header
                 COMMAND nfsmw_exhaust_c_header_test)
    endif()

    if(WIN32)
        add_executable(nfsmw_exhaust_startup_gate_tests
            tests/StartupGateTests.cpp
            src/StartupGate.cpp
        )
        target_include_directories(nfsmw_exhaust_startup_gate_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(nfsmw_exhaust_startup_gate_tests PRIVATE
            advapi32
            crypt32)
        target_compile_features(nfsmw_exhaust_startup_gate_tests PRIVATE cxx_std_17)
        if(MSVC)
            target_compile_options(nfsmw_exhaust_startup_gate_tests PRIVATE /W4 /EHsc)
            target_compile_definitions(nfsmw_exhaust_startup_gate_tests PRIVATE
                WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
        endif()
        add_test(NAME nfsmw_exhaust_startup_gate
                 COMMAND nfsmw_exhaust_startup_gate_tests)

        add_executable(nfsmw_exhaust_sunset_lighting_tests
            tests/SunSetLightingTests.cpp
            src/SunSetLighting.cpp
        )
        target_include_directories(nfsmw_exhaust_sunset_lighting_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_features(nfsmw_exhaust_sunset_lighting_tests PRIVATE
            cxx_std_17)
        if(MSVC)
            target_compile_options(nfsmw_exhaust_sunset_lighting_tests PRIVATE
                /W4 /EHsc)
        endif()
        add_test(NAME nfsmw_exhaust_sunset_lighting
                 COMMAND nfsmw_exhaust_sunset_lighting_tests)

        if(TARGET nfsmw_exhaust_native)
            add_executable(nfsmw_exhaust_native_audio_tests
                tests/NativeAudioTests.cpp
                src/NativeAudio.cpp
                src/NativeLog.cpp
            )
            target_include_directories(nfsmw_exhaust_native_audio_tests PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src)
            target_link_libraries(nfsmw_exhaust_native_audio_tests PRIVATE
                ole32
                xaudio2)
            target_compile_features(nfsmw_exhaust_native_audio_tests PRIVATE
                cxx_std_17)
            if(MSVC)
                target_compile_options(nfsmw_exhaust_native_audio_tests PRIVATE
                    /W4 /EHsc /utf-8)
                target_compile_definitions(nfsmw_exhaust_native_audio_tests PRIVATE
                    WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
                    NFSMW_EXHAUST_ENABLE_LOGGING=1)
            endif()
            add_test(NAME nfsmw_exhaust_native_audio
                     COMMAND nfsmw_exhaust_native_audio_tests
                             "${CMAKE_CURRENT_SOURCE_DIR}")
        endif()
    endif()
endif()
