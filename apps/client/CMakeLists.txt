option(URT_CLIENT_WINDOW "Build the client with a window (needs URT_BUILD_RENDERER)" ON)

add_executable(urt_client main.cpp)
target_link_libraries(urt_client PRIVATE urt::engine urt_options)

if(URT_BUILD_RENDERER)
    # What the client draws, without the window: also used by a test.
    add_library(urt_client_view STATIC world_view.cpp)
    target_include_directories(urt_client_view PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(urt_client_view PUBLIC urt::engine urt::render PRIVATE urt_options)
endif()

if(URT_CLIENT_WINDOW AND URT_BUILD_RENDERER)
    # GLFW - creates the window and reads the keyboard.
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG        3.4
        GIT_SHALLOW    TRUE
        SYSTEM)
    FetchContent_MakeAvailable(glfw)

    target_sources(urt_client PRIVATE game_window.cpp)
    target_compile_definitions(urt_client PRIVATE URT_CLIENT_WINDOW=1)
    target_link_libraries(urt_client PRIVATE urt_client_view glfw)
endif()
