cmake_minimum_required(VERSION 3.20)

project(nfsmw_drift_assist_rigidbody_companion LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR
        "The rigid-body companion must be built as a Win32 ASI on Windows.")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "NFSMW 2005 is a 32-bit process. Configure this target with an x86 compiler.")
endif()

set(NFSMW_SDK_SOURCE_DIR "" CACHE PATH
    "Source tree of nfsmw-2005-sdk")
if(NOT NFSMW_SDK_SOURCE_DIR OR
   NOT EXISTS "${NFSMW_SDK_SOURCE_DIR}/include/nfsmw_sdk/nfsmw_sdk.h" OR
   NOT EXISTS "${NFSMW_SDK_SOURCE_DIR}/src/entry.c")
    message(FATAL_ERROR
        "NFSMW_SDK_SOURCE_DIR must point to the nfsmw-2005-sdk source tree")
endif()

set(NFSMW_DRIFT_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

add_library(nfsmw_drift_assist_rigidbody_companion SHARED
    rigidbody_companion.cpp
    "${NFSMW_SDK_SOURCE_DIR}/src/entry.c"
    "${NFSMW_DRIFT_ROOT}/drift_assist.cpp")

target_include_directories(nfsmw_drift_assist_rigidbody_companion PRIVATE
    "${NFSMW_SDK_SOURCE_DIR}/include"
    "${NFSMW_DRIFT_ROOT}")
target_compile_features(nfsmw_drift_assist_rigidbody_companion PRIVATE cxx_std_17)
target_compile_definitions(nfsmw_drift_assist_rigidbody_companion PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX)
target_link_libraries(nfsmw_drift_assist_rigidbody_companion PRIVATE kernel32)

if(MSVC)
    target_compile_options(nfsmw_drift_assist_rigidbody_companion PRIVATE
        /W4 /permissive- /EHsc /utf-8)
    set_property(TARGET nfsmw_drift_assist_rigidbody_companion PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(nfsmw_drift_assist_rigidbody_companion PRIVATE
        -Wall -Wextra -Wpedantic)
endif()

set_target_properties(nfsmw_drift_assist_rigidbody_companion PROPERTIES
    PREFIX ""
    SUFFIX ".dll"
    CXX_EXTENSIONS OFF)

add_custom_command(TARGET nfsmw_drift_assist_rigidbody_companion POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:nfsmw_drift_assist_rigidbody_companion>"
        "$<TARGET_FILE_DIR:nfsmw_drift_assist_rigidbody_companion>/nfsmw_drift_assist_rigidbody.asi"
    COMMENT "Copying the rigid-body companion DLL to an ASI")
