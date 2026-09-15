cmake_minimum_required(VERSION 3.20)

project(nfsmw_drift_assist LANGUAGES C CXX)

add_library(nfsmw_drift_assist STATIC
    drift_assist.cpp
    asi_host/driver_assist.cpp
    asi_host/driver_assist_hud_layout.cpp
)
target_include_directories(nfsmw_drift_assist PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_features(nfsmw_drift_assist PUBLIC cxx_std_17)
set_target_properties(nfsmw_drift_assist PROPERTIES
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(nfsmw_drift_assist PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(nfsmw_drift_assist PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(nfsmw_drift_assist_tests
        drift_assist_tests.cpp
    )
    target_link_libraries(nfsmw_drift_assist_tests PRIVATE nfsmw_drift_assist)
    target_compile_features(nfsmw_drift_assist_tests PRIVATE cxx_std_17)

    if(MSVC)
        target_compile_options(nfsmw_drift_assist_tests PRIVATE
            /W4 /WX /permissive- /EHsc)
    else()
        target_compile_options(nfsmw_drift_assist_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_test(NAME nfsmw_drift_assist_tests COMMAND nfsmw_drift_assist_tests)

    add_executable(nfsmw_drift_requirements_tests
        drift_requirements_tests.cpp
    )
    target_link_libraries(nfsmw_drift_requirements_tests PRIVATE nfsmw_drift_assist)
    target_compile_features(nfsmw_drift_requirements_tests PRIVATE cxx_std_17)

    if(MSVC)
        target_compile_options(nfsmw_drift_requirements_tests PRIVATE
            /W4 /WX /permissive- /EHsc)
    else()
        target_compile_options(nfsmw_drift_requirements_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_test(NAME nfsmw_drift_requirements_tests COMMAND nfsmw_drift_requirements_tests)

    add_executable(nfsmw_drift_camera_tests
        drift_camera_tests.cpp
    )
    target_link_libraries(nfsmw_drift_camera_tests PRIVATE nfsmw_drift_assist)
    target_compile_features(nfsmw_drift_camera_tests PRIVATE cxx_std_17)

    if(MSVC)
        target_compile_options(nfsmw_drift_camera_tests PRIVATE
            /W4 /WX /permissive- /EHsc)
    else()
        target_compile_options(nfsmw_drift_camera_tests PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()

    add_test(NAME nfsmw_drift_camera_tests COMMAND nfsmw_drift_camera_tests)

    add_executable(nfsmw_drift_steering_response_tests
        steering_response_tests.cpp
    )
    target_link_libraries(nfsmw_drift_steering_response_tests
        PRIVATE nfsmw_drift_assist)
    target_compile_features(nfsmw_drift_steering_response_tests
        PRIVATE cxx_std_17)

    if(MSVC)
        target_compile_options(nfsmw_drift_steering_response_tests PRIVATE
            /W4 /WX /permissive- /EHsc)
    else()
        target_compile_options(nfsmw_drift_steering_response_tests PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()

    add_test(NAME nfsmw_drift_steering_response_tests
             COMMAND nfsmw_drift_steering_response_tests)

    add_executable(nfsmw_drift_handling_probe_state_tests
        handling_probe_state_tests.cpp
    )
    target_link_libraries(nfsmw_drift_handling_probe_state_tests
        PRIVATE nfsmw_drift_assist)
    target_compile_features(nfsmw_drift_handling_probe_state_tests
        PRIVATE cxx_std_17)

    if(MSVC)
        target_compile_options(nfsmw_drift_handling_probe_state_tests PRIVATE
            /W4 /WX /permissive- /EHsc)
    else()
        target_compile_options(nfsmw_drift_handling_probe_state_tests PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()

    add_test(NAME nfsmw_drift_handling_probe_state_tests
             COMMAND nfsmw_drift_handling_probe_state_tests)

    add_executable(nfsmw_rigidbody_accel_experiment_tests
        rigidbody_accel_experiment_tests.cpp
    )
    target_link_libraries(nfsmw_rigidbody_accel_experiment_tests
        PRIVATE nfsmw_drift_assist)
    target_compile_features(nfsmw_rigidbody_accel_experiment_tests
        PRIVATE cxx_std_17)

    if(MSVC)
        target_compile_options(nfsmw_rigidbody_accel_experiment_tests PRIVATE
            /W4 /WX /permissive- /EHsc)
    else()
        target_compile_options(nfsmw_rigidbody_accel_experiment_tests PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()

    add_test(NAME nfsmw_rigidbody_accel_experiment_tests
             COMMAND nfsmw_rigidbody_accel_experiment_tests)

    add_executable(nfsmw_vehicle_countersteer_tests
        vehicle_countersteer_tests.cpp
    )
    target_link_libraries(nfsmw_vehicle_countersteer_tests
        PRIVATE nfsmw_drift_assist)
    target_compile_features(nfsmw_vehicle_countersteer_tests
        PRIVATE cxx_std_17)

    if(MSVC)
        target_compile_options(nfsmw_vehicle_countersteer_tests PRIVATE
            /W4 /WX /permissive- /EHsc)
    else()
        target_compile_options(nfsmw_vehicle_countersteer_tests PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()

    add_test(NAME nfsmw_vehicle_countersteer_tests
             COMMAND nfsmw_vehicle_countersteer_tests)

    add_executable(nfsmw_rear_wheel_steering_tests
        rear_wheel_steering_tests.cpp
    )
    target_link_libraries(nfsmw_rear_wheel_steering_tests
        PRIVATE nfsmw_drift_assist)
    target_compile_features(nfsmw_rear_wheel_steering_tests
        PRIVATE cxx_std_17)

    if(MSVC)
        target_compile_options(nfsmw_rear_wheel_steering_tests PRIVATE
            /W4 /WX /permissive- /EHsc)
    else()
        target_compile_options(nfsmw_rear_wheel_steering_tests PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()

    add_test(NAME nfsmw_rear_wheel_steering_tests
             COMMAND nfsmw_rear_wheel_steering_tests)

    add_executable(nfsmw_drift_drive_torque_tests
        drift_drive_torque_tests.cpp)
    target_compile_features(nfsmw_drift_drive_torque_tests PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(nfsmw_drift_drive_torque_tests PRIVATE /W4 /WX)
    endif()
    add_test(NAME nfsmw_drift_drive_torque_tests
             COMMAND nfsmw_drift_drive_torque_tests)

    add_executable(nfsmw_driver_assist_tests
        driver_assist_tests.cpp
    )
    target_link_libraries(nfsmw_driver_assist_tests
        PRIVATE nfsmw_drift_assist)
    target_compile_features(nfsmw_driver_assist_tests PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(nfsmw_driver_assist_tests PRIVATE
            /W4 /WX /permissive- /EHsc)
    else()
        target_compile_options(nfsmw_driver_assist_tests PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME nfsmw_driver_assist_tests COMMAND nfsmw_driver_assist_tests)

    add_executable(nfsmw_driver_assist_hud_tests
        driver_assist_hud_tests.cpp
    )
    target_link_libraries(nfsmw_driver_assist_hud_tests
        PRIVATE nfsmw_drift_assist)
    target_compile_features(nfsmw_driver_assist_hud_tests PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(nfsmw_driver_assist_hud_tests PRIVATE
            /W4 /WX /permissive- /EHsc)
    else()
        target_compile_options(nfsmw_driver_assist_hud_tests PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME nfsmw_driver_assist_hud_tests
             COMMAND nfsmw_driver_assist_hud_tests)

    if(WIN32)
        set(NFSMW_REFORGED_TEST_EXE "" CACHE FILEPATH
            "Optional stamped Reforged executable used by startup-gate integration tests")
        add_executable(nfsmw_drift_reforged_startup_gate_tests
            reforged_startup_gate_tests.cpp
            asi_host/reforged_startup_gate.cpp
        )
        target_include_directories(nfsmw_drift_reforged_startup_gate_tests
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/asi_host")
        target_compile_features(nfsmw_drift_reforged_startup_gate_tests
            PRIVATE cxx_std_17)
        target_link_libraries(nfsmw_drift_reforged_startup_gate_tests
            PRIVATE advapi32 crypt32)
        if(MSVC)
            target_compile_options(nfsmw_drift_reforged_startup_gate_tests
                PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
            set_property(TARGET nfsmw_drift_reforged_startup_gate_tests
                PROPERTY MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        else()
            target_compile_options(nfsmw_drift_reforged_startup_gate_tests
                PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        add_test(NAME nfsmw_drift_reforged_startup_gate_tests
                 COMMAND nfsmw_drift_reforged_startup_gate_tests)
        if(NFSMW_REFORGED_TEST_EXE)
            if(NOT EXISTS "${NFSMW_REFORGED_TEST_EXE}")
                message(FATAL_ERROR
                    "NFSMW_REFORGED_TEST_EXE does not exist: ${NFSMW_REFORGED_TEST_EXE}")
            endif()
            add_test(NAME nfsmw_drift_reforged_executable_tests
                COMMAND nfsmw_drift_reforged_startup_gate_tests
                        "${NFSMW_REFORGED_TEST_EXE}")
        endif()
    endif()
endif()

# Optional, deliberately fail-closed 32-bit ASI/DLL bootstrap.  The bootstrap
# links the controller and the upstream nfsmw-2005-sdk entry shim, but does not
# install any game hook until the caller supplies a target-specific anchor
# signature.  Keeping this opt-in means the portable controller tests remain
# usable on a host that has no Win32 SDK or game executable.
option(NFSMW_BUILD_ASI_HOST
       "Build the x86 ASI/DLL bootstrap (requires nfsmw-2005-sdk)" OFF)
option(NFSMW_ENABLE_INPUT_DIAGNOSTIC
       "Build/install the read-only input poll diagnostic hook (requires a verified anchor)" OFF)
option(NFSMW_ENABLE_VEHICLE_DIAGNOSTIC
       "Build/install the read-only ActiveComponents vehicle diagnostic hook (requires a verified anchor)" OFF)
option(NFSMW_ENABLE_COORDINATOR_DIAGNOSTIC
       "Build/install the read-only physics coordinator diagnostic hook (requires a verified anchor)" OFF)
option(NFSMW_ENABLE_PHASE_DIAGNOSTIC
       "Build/install the mutually exclusive read-only two-anchor phase/timing diagnostic" OFF)
option(NFSMW_ENABLE_ALPHA_BRIDGE
       "Build/install the bounded smart-countersteer/manual-yaw alpha bridge" OFF)
option(NFSMW_ENABLE_RIGIDBODY_ACCEL_EXPERIMENT
       "Build the forward rigid-body acceleration channel (dry-run unless write is enabled)" OFF)
option(NFSMW_ENABLE_RIGIDBODY_ACCEL_WRITE
       "Enable the reviewed rigid-body writer (unsafe; release build opts in explicitly)" OFF)
option(NFSMW_RELEASE_SILENT
       "Disable runtime DebugView and file logging in the official release" OFF)
option(NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE
       "Write a bounded rear-wheel-steering diagnostic log next to the ASI" OFF)
option(NFSMW_ENABLE_REAR_WHEEL_STEERING
       "Enable player-qualified visual and physical rear-wheel steering" OFF)
option(NFSMW_ENABLE_RWS_FORCE_TRACE
       "Trace player wheel force inputs/outputs without changing game state" OFF)
if(NFSMW_ENABLE_RWS_FORCE_TRACE AND
   (NOT NFSMW_ENABLE_ALPHA_BRIDGE OR NFSMW_ENABLE_REAR_WHEEL_STEERING))
    message(FATAL_ERROR
        "RWS force trace requires the player-qualified alpha bridge and "
        "must not run with the unfinished rear-wheel-steering prototype")
endif()
if(NFSMW_ENABLE_RIGIDBODY_ACCEL_WRITE AND
   NOT NFSMW_ENABLE_RIGIDBODY_ACCEL_EXPERIMENT)
    message(FATAL_ERROR
        "NFSMW_ENABLE_RIGIDBODY_ACCEL_WRITE requires "
        "NFSMW_ENABLE_RIGIDBODY_ACCEL_EXPERIMENT")
endif()
if((NFSMW_ENABLE_RIGIDBODY_ACCEL_EXPERIMENT OR
    NFSMW_ENABLE_RIGIDBODY_ACCEL_WRITE) AND
   NOT NFSMW_ENABLE_ALPHA_BRIDGE)
    message(FATAL_ERROR
        "The rigid-body acceleration experiment requires "
        "NFSMW_ENABLE_ALPHA_BRIDGE=ON")
endif()
set(NFSMW_SDK_SOURCE_DIR "" CACHE PATH
    "Source tree of s-b-repo/nfsmw-2005-sdk (or leave empty for find_package)")

if(NFSMW_BUILD_ASI_HOST)
    if(NOT WIN32)
        message(FATAL_ERROR
            "NFSMW_BUILD_ASI_HOST requires a Windows/Win32 PE build. "
            "Use an i686 MinGW toolchain when cross-compiling.")
    endif()
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
        message(FATAL_ERROR
            "NFSMW_BUILD_ASI_HOST requires a 32-bit (x86) compiler; "
            "NFSMW is an i386 process.")
    endif()

    if(NFSMW_SDK_SOURCE_DIR)
        if(NOT EXISTS "${NFSMW_SDK_SOURCE_DIR}/CMakeLists.txt")
            message(FATAL_ERROR
                "NFSMW_SDK_SOURCE_DIR does not contain nfsmw-2005-sdk/CMakeLists.txt")
        endif()
        # The SDK examples are unrelated to this plugin and would duplicate
        # targets when the SDK is embedded in this project.
        set(NFSMW_BUILD_EXAMPLES OFF CACHE BOOL
            "Disable upstream nfsmw examples for the drift host" FORCE)
        add_subdirectory("${NFSMW_SDK_SOURCE_DIR}"
                         "${CMAKE_BINARY_DIR}/_deps/nfsmw_sdk")

        # nfsmw_add_plugin() is declared as a CMake function.  Functions
        # resolve ordinary variables in their caller scope, while the SDK
        # declares these paths in the add_subdirectory scope.  Mirror the
        # paths here so the helper does not silently produce `-I\\include`
        # and omit the entry/MinHook sources when the SDK is embedded.
        set(NFSMW_SDK_INCLUDE_DIR
            "${NFSMW_SDK_SOURCE_DIR}/include")
        set(NFSMW_SDK_ENTRY_SOURCE
            "${NFSMW_SDK_SOURCE_DIR}/src/entry.c")
        set(NFSMW_SDK_MINHOOK_DIR
            "${NFSMW_SDK_SOURCE_DIR}/extern/minhook")
        set(NFSMW__MINHOOK_SRCS
            "${NFSMW_SDK_MINHOOK_DIR}/src/buffer.c"
            "${NFSMW_SDK_MINHOOK_DIR}/src/hook.c"
            "${NFSMW_SDK_MINHOOK_DIR}/src/trampoline.c"
            "${NFSMW_SDK_MINHOOK_DIR}/src/hde/hde32.c"
            "${NFSMW_SDK_MINHOOK_DIR}/src/hde/hde64.c")
    else()
        find_package(nfsmw_sdk CONFIG REQUIRED)
    endif()

    # These PE fields match the exact stamped Reforged image launched as speed.exe.
    # The startup gate separately verifies its complete hashes, 64-byte footer,
    # embedded 868086 code, and canonical original-image digest before this
    # in-memory profile and its required hook anchors are inspected.
    set(NFSMW_DRIFT_EXPECTED_MACHINE "0x014c" CACHE STRING
        "Expected PE machine field (IMAGE_FILE_MACHINE_I386)")
    set(NFSMW_DRIFT_EXPECTED_IMAGE_BASE "0x00400000" CACHE STRING
        "Expected speed.exe image base")
    set(NFSMW_DRIFT_EXPECTED_ENTRY_RVA "0x003c4040" CACHE STRING
        "Expected mainCRTStartup RVA for the supported profile")
    set(NFSMW_DRIFT_EXPECTED_SIZE_OF_IMAGE "0x00693000" CACHE STRING
        "Expected SizeOfImage (0 disables this optional PE-field check)")
    set(NFSMW_DRIFT_EXPECTED_FILE_SIZE "0x005da040" CACHE STRING
        "Expected on-disk stamped Reforged executable size")
    set(NFSMW_DRIFT_REQUIRED_SIGNATURE_RVA "0" CACHE STRING
        "Verified code-anchor RVA; zero keeps the bootstrap disabled")
    set(NFSMW_DRIFT_REQUIRED_SIGNATURE "" CACHE STRING
        "Verified IDA-style anchor bytes, e.g. '55 8B EC 83 EC ??'")
    set(NFSMW_DRIFT_PHASE_WORLD_SIGNATURE_RVA "0" CACHE STRING
        "Second verified phase-diagnostic anchor RVA; zero keeps that diagnostic disabled")
    set(NFSMW_DRIFT_PHASE_WORLD_SIGNATURE "" CACHE STRING
        "Second verified phase-diagnostic IDA-style anchor bytes")
    set(NFSMW_DRIFT_ANGULAR_VELOCITY_SETTER_RVA "0" CACHE STRING
        "Verified IRigidBody::SetAngularVelocity RVA; zero disables the alpha bridge")
    set(NFSMW_DRIFT_ANGULAR_VELOCITY_SETTER_SIGNATURE "" CACHE STRING
        "Verified IRigidBody::SetAngularVelocity IDA-style anchor bytes")
    set(NFSMW_DRIFT_TIRE_FORCE_RVA "0x0029DA90" CACHE STRING
        "Profile-specific read-only tire-force probe entry RVA")
    set(NFSMW_DRIFT_TIRE_FORCE_SIGNATURE
        "83 EC 24 56 8B F1 8B 86 0C 01 00 00 8B 8E 04 01 00 00 8B 49 08 D9 44 81 08"
        CACHE STRING "Unique IDA-style tire-force probe entry signature")
    set(NFSMW_DRIFT_TIRE_FORCE_CALL_RVA "0x002AA8F8" CACHE STRING
        "Profile-specific unique tire-force call-site RVA")
    set(NFSMW_DRIFT_TIRE_FORCE_CALL_SIGNATURE "E8 93 31 FF FF" CACHE STRING
        "Unique IDA-style tire-force call-site signature")
    set(NFSMW_DRIFT_DRIVE_TORQUE_RVA "0x002A0580" CACHE STRING
        "Profile-specific ITransmission::GetDriveTorque RVA")
    set(NFSMW_DRIFT_DRIVE_TORQUE_SIGNATURE "D9 41 34 C3" CACHE STRING
        "Exact ITransmission::GetDriveTorque entry bytes; runtime vtable validation is also required")

    set(_nfsmw_diagnostic_count 0)
    foreach(_nfsmw_diagnostic_option
            NFSMW_ENABLE_INPUT_DIAGNOSTIC
            NFSMW_ENABLE_VEHICLE_DIAGNOSTIC
        NFSMW_ENABLE_COORDINATOR_DIAGNOSTIC
        NFSMW_ENABLE_PHASE_DIAGNOSTIC
            NFSMW_ENABLE_ALPHA_BRIDGE)
        if(${_nfsmw_diagnostic_option})
            math(EXPR _nfsmw_diagnostic_count
                 "${_nfsmw_diagnostic_count} + 1")
        endif()
    endforeach()
    if(_nfsmw_diagnostic_count GREATER 1)
        message(FATAL_ERROR
            "Input, vehicle, coordinator, phase/timing diagnostics, and the alpha bridge are "
            "mutually exclusive and must be built separately")
    endif()

    if((NFSMW_ENABLE_INPUT_DIAGNOSTIC OR NFSMW_ENABLE_VEHICLE_DIAGNOSTIC OR
        NFSMW_ENABLE_COORDINATOR_DIAGNOSTIC OR
        NFSMW_ENABLE_PHASE_DIAGNOSTIC OR NFSMW_ENABLE_ALPHA_BRIDGE) AND
       (NFSMW_DRIFT_REQUIRED_SIGNATURE_RVA STREQUAL "0" OR
        NFSMW_DRIFT_REQUIRED_SIGNATURE STREQUAL ""))
        message(FATAL_ERROR
            "A diagnostic option requires "
            "NFSMW_DRIFT_REQUIRED_SIGNATURE_RVA and "
            "NFSMW_DRIFT_REQUIRED_SIGNATURE measured for the exact speed.exe")
    endif()
    if((NFSMW_ENABLE_PHASE_DIAGNOSTIC OR NFSMW_ENABLE_ALPHA_BRIDGE) AND
       (NFSMW_DRIFT_PHASE_WORLD_SIGNATURE_RVA STREQUAL "0" OR
        NFSMW_DRIFT_PHASE_WORLD_SIGNATURE STREQUAL ""))
        message(FATAL_ERROR
            "The phase diagnostic and alpha bridge additionally require "
            "NFSMW_DRIFT_PHASE_WORLD_SIGNATURE_RVA and "
            "NFSMW_DRIFT_PHASE_WORLD_SIGNATURE")
    endif()
    if(NFSMW_ENABLE_ALPHA_BRIDGE AND
       (NFSMW_DRIFT_ANGULAR_VELOCITY_SETTER_RVA STREQUAL "0" OR
        NFSMW_DRIFT_ANGULAR_VELOCITY_SETTER_SIGNATURE STREQUAL ""))
        message(FATAL_ERROR
            "The alpha bridge additionally requires the verified "
            "NFSMW_DRIFT_ANGULAR_VELOCITY_SETTER_RVA and "
            "NFSMW_DRIFT_ANGULAR_VELOCITY_SETTER_SIGNATURE")
    endif()

    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/asi_host/nfsmw_drift_target_profile.hpp.in"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/nfsmw_drift_target_profile.hpp"
        @ONLY)

    # The generic SDK entry accepts only speed.exe and would reject the renamed
    # process before the full Reforged/startup gate can run. This project-local
    # shim performs only the exact basename/load-base check outside loader lock;
    # the C++ startup gate then performs all cryptographic and device checks.
    set(NFSMW_SDK_ENTRY_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/asi_host/reforged_entry.c")

    nfsmw_add_plugin(nfsmw_drift_assist_asi
                     SOURCES asi_host/asi_main.cpp
                              asi_host/reforged_startup_gate.cpp
                              asi_host/vehicle_getter_diagnostic.cpp
                              asi_host/coordinator_diagnostic.cpp
                              asi_host/phase_timing_diagnostic.cpp
                              asi_host/alpha_bridge.cpp
                              asi_host/driver_assist_hud.cpp
                              asi_host/handling_probe.cpp)
    target_link_libraries(nfsmw_drift_assist_asi PRIVATE
        nfsmw_drift_assist advapi32 crypt32)
    target_compile_definitions(nfsmw_drift_assist_asi PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        NFSMW_ENABLE_INPUT_DIAGNOSTIC=$<BOOL:${NFSMW_ENABLE_INPUT_DIAGNOSTIC}>
        NFSMW_ENABLE_VEHICLE_DIAGNOSTIC=$<BOOL:${NFSMW_ENABLE_VEHICLE_DIAGNOSTIC}>
        NFSMW_ENABLE_COORDINATOR_DIAGNOSTIC=$<BOOL:${NFSMW_ENABLE_COORDINATOR_DIAGNOSTIC}>
        NFSMW_ENABLE_PHASE_DIAGNOSTIC=$<BOOL:${NFSMW_ENABLE_PHASE_DIAGNOSTIC}>
        NFSMW_ENABLE_ALPHA_BRIDGE=$<BOOL:${NFSMW_ENABLE_ALPHA_BRIDGE}>
        NFSMW_ENABLE_RIGIDBODY_ACCEL_EXPERIMENT=$<BOOL:${NFSMW_ENABLE_RIGIDBODY_ACCEL_EXPERIMENT}>
        NFSMW_ENABLE_RIGIDBODY_ACCEL_WRITE=$<BOOL:${NFSMW_ENABLE_RIGIDBODY_ACCEL_WRITE}>
        NFSMW_RELEASE_SILENT=$<BOOL:${NFSMW_RELEASE_SILENT}>
        NFSMW_ENABLE_REAR_WHEEL_STEERING=$<BOOL:${NFSMW_ENABLE_REAR_WHEEL_STEERING}>
        NFSMW_ENABLE_RWS_FORCE_TRACE=$<BOOL:${NFSMW_ENABLE_RWS_FORCE_TRACE}>
        NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE=$<BOOL:${NFSMW_ENABLE_RWS_DIAGNOSTIC_FILE}>)
    if(MSVC)
        # The static controller is linked into the DLL.  Keep its CRT choice
        # identical to the plugin to avoid /MD-versus-/MT ABI conflicts.
        set_property(TARGET nfsmw_drift_assist PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    target_include_directories(nfsmw_drift_assist_asi PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_compile_features(nfsmw_drift_assist_asi PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(nfsmw_drift_assist_asi PRIVATE
            /W4 /permissive- /EHsc /utf-8)
        if(NFSMW_ENABLE_PHASE_DIAGNOSTIC OR NFSMW_ENABLE_INPUT_DIAGNOSTIC OR
           NFSMW_ENABLE_ALPHA_BRIDGE)
            # The target also embeds third-party MinHook C sources with known
            # legacy warnings. Keep all project C++ at /WX without turning
            # those vendored C warnings into unrelated build failures.
            target_compile_options(nfsmw_drift_assist_asi PRIVATE
                $<$<COMPILE_LANGUAGE:CXX>:/WX>)
        endif()
        # Match the SDK's documented single-file recipe; avoid a dependency on
        # the user's VC++ redistributable in the generated ASI.
        set_property(TARGET nfsmw_drift_assist_asi PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        target_compile_options(nfsmw_drift_assist_asi PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    if(NFSMW_ENABLE_PHASE_DIAGNOSTIC)
        add_custom_command(TARGET nfsmw_drift_assist_asi POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:nfsmw_drift_assist_asi>"
                "$<TARGET_FILE_DIR:nfsmw_drift_assist_asi>/nfsmw_drift_assist_phase_timing_diag.asi"
            COMMENT "Copying the standalone phase/timing diagnostic ASI")
    endif()
    if(NFSMW_ENABLE_ALPHA_BRIDGE)
        # The release is a single, deliberately branded ASI.  Keep the CMake
        # target name internal, but emit only the filename accepted by the
        # distribution package so an old companion/alpha artifact cannot be
        # mistaken for the integrated plugin.
        add_custom_command(TARGET nfsmw_drift_assist_asi POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:nfsmw_drift_assist_asi>"
                "$<TARGET_FILE_DIR:nfsmw_drift_assist_asi>/Slippery_Drifting_FlashFish_by_AndyQinke.asi"
            COMMAND ${CMAKE_COMMAND} -E rm -f
                "$<TARGET_FILE_DIR:nfsmw_drift_assist_asi>/nfsmw_drift_assist_asi.asi"
            COMMENT "Copying the integrated Slippery_Drifting_FlashFish_by_AndyQinke ASI")
    endif()
endif()
