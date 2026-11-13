# Third-party libraries, downloaded at configure time and pinned to exact tags
# so every machine builds the same code.

include(FetchContent)
find_package(Threads REQUIRED)

# EnTT - entity component system (header only).
FetchContent_Declare(entt
    GIT_REPOSITORY https://github.com/skypjack/entt.git
    GIT_TAG        v3.16.0
    GIT_SHALLOW    TRUE
    SYSTEM)

# GLM - vector and matrix math (header only), same conventions as GLSL.
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.3
    GIT_SHALLOW    TRUE
    SYSTEM)

# Header-only GLM: nothing to compile, and no warnings from its sources.
set(GLM_BUILD_LIBRARY OFF CACHE BOOL "" FORCE)

# ENet - reliable and unreliable messaging over UDP (C library).
FetchContent_Declare(enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG        v1.3.18
    GIT_SHALLOW    TRUE
    SYSTEM)

# Luau - the scripting language for resources: a Lua dialect built to run
# code you do not trust.
set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(luau
    GIT_REPOSITORY https://github.com/luau-lang/luau.git
    GIT_TAG        0.746
    GIT_SHALLOW    TRUE
    # A folder that does not exist: this stops FetchContent from adding the
    # project by itself, so we can add it below with EXCLUDE_FROM_ALL.
    SOURCE_SUBDIR  urt-adds-this-manually)

FetchContent_MakeAvailable(entt glm enet luau)

# Luau also contains a type checker and a native code generator. With
# EXCLUDE_FROM_ALL only the parts we link (VM and compiler) get built.
add_subdirectory("${luau_SOURCE_DIR}" "${luau_BINARY_DIR}" EXCLUDE_FROM_ALL SYSTEM)

# ENet's own CMake file does not say where its headers are or which system
# libraries it needs on Windows, so add both here.
target_include_directories(enet SYSTEM PUBLIC "${enet_SOURCE_DIR}/include")
if(WIN32)
    target_link_libraries(enet PUBLIC ws2_32 winmm)
endif()

if(URT_BUILD_TESTS)
    # doctest - unit test framework (header only).
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG        v2.5.3
        GIT_SHALLOW    TRUE
    SYSTEM)
    set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(doctest)
endif()
