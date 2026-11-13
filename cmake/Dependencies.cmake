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

FetchContent_MakeAvailable(entt glm enet)

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
