cmake_minimum_required(VERSION 3.20)

project(nfsmw_drift_assist_asi LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR
        "The ASI host must be built as a Windows PE DLL. Use Visual Studio on Windows.")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "NFSMW 2005 is a 32-bit process. Configure this project with -A Win32.")
endif()

set(DRIFT_CORE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." CACHE PATH
    "Directory containing drift_assist.cpp and its headers")

# This switch is intentionally opt-in and currently only exposes a diagnostic
# path. No unverified address is installed by this starter host.
option(NFSMW_ENABLE_UNVERIFIED_HOOKS
    "Allow future, explicitly reviewed game hooks (unsafe; keep OFF)" OFF)

add_library(nfsmw_drift_assist_asi SHARED
    asi_entry.cpp
    version_guard.cpp
    "${DRIFT_CORE_ROOT}/drift_assist.cpp"
)

target_include_directories(nfsmw_drift_assist_asi PRIVATE
    "${DRIFT_CORE_ROOT}"
    "${CMAKE_CURRENT_LIST_DIR}"
)
target_compile_features(nfsmw_drift_assist_asi PRIVATE cxx_std_17)
target_compile_definitions(nfsmw_drift_assist_asi PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    NFSMW_ENABLE_UNVERIFIED_HOOKS=$<BOOL:${NFSMW_ENABLE_UNVERIFIED_HOOKS}>
)
target_link_libraries(nfsmw_drift_assist_asi PRIVATE kernel32)

if(MSVC)
    target_compile_options(nfsmw_drift_assist_asi PRIVATE /W4 /permissive- /EHsc)
    # Static CRT keeps the plugin independent of the user's VC++ runtime.
    set_property(TARGET nfsmw_drift_assist_asi PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(nfsmw_drift_assist_asi PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(nfsmw_drift_assist_asi PROPERTIES
    PREFIX ""
    SUFFIX ".dll"
    CXX_EXTENSIONS OFF
)

# Ultimate ASI Loader loads the same PE image under the .asi suffix.
add_custom_command(TARGET nfsmw_drift_assist_asi POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:nfsmw_drift_assist_asi>"
        "$<TARGET_FILE_DIR:nfsmw_drift_assist_asi>/nfsmw_drift_assist_asi.asi"
    COMMENT "Copying nfsmw_drift_assist_asi.dll -> nfsmw_drift_assist_asi.asi")
