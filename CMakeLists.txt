cmake_minimum_required(VERSION 3.16)
project(MWDriftController LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(MWDriftController SHARED
  src/plugin.cpp
)

target_compile_definitions(MWDriftController PRIVATE UNICODE _UNICODE)

if(MSVC)
  target_compile_options(MWDriftController PRIVATE /utf-8 /W4 /EHsc)
  set_target_properties(MWDriftController PROPERTIES
    OUTPUT_NAME "MW Drift Controller by AndyQinke"
    SUFFIX ".asi"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  )
else()
  set_target_properties(MWDriftController PROPERTIES
    OUTPUT_NAME "MW Drift Controller by AndyQinke"
    PREFIX ""
    SUFFIX ".asi"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  )
  target_link_libraries(MWDriftController PRIVATE -static-libgcc -static-libstdc++)
endif()

target_link_libraries(MWDriftController PRIVATE user32)
