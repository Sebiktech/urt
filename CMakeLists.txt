cmake_minimum_required(VERSION 3.25)

project(URT
    VERSION 0.2.0
    DESCRIPTION "Universal Roleplay Tool - engine, game and editor"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# All executables land in build/<preset>/bin so they are easy to find.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(URT_BUILD_TESTS "Build unit tests" ON)
set(URT_SANITIZE "" CACHE STRING "Sanitizer to enable: address, thread or empty")

include(cmake/CompilerOptions.cmake)
include(cmake/Dependencies.cmake)

add_subdirectory(engine)
add_subdirectory(apps/server)
add_subdirectory(apps/client)

if(URT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
