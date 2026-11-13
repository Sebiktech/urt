option(URT_CLIENT_WINDOW "Build the client's debug window (needs OpenGL)" ON)

add_executable(urt_client main.cpp)
target_link_libraries(urt_client PRIVATE urt::engine urt_options)

if(URT_CLIENT_WINDOW)
    find_package(OpenGL REQUIRED)

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

    target_sources(urt_client PRIVATE debug_view.cpp)
    target_compile_definitions(urt_client PRIVATE URT_CLIENT_WINDOW=1)
    target_link_libraries(urt_client PRIVATE glfw OpenGL::GL)
endif()
