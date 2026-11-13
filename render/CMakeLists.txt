# urt_render: the Vulkan renderer. Only the client links it; the server and
# urt_engine stay free of graphics.
#
# Needs the Vulkan SDK (https://vulkan.lunarg.com) for the headers and the
# loader library.

find_package(Vulkan REQUIRED)

add_library(urt_render STATIC src/renderer.cpp)
add_library(urt::render ALIAS urt_render)

target_include_directories(urt_render PUBLIC include PRIVATE shaders)
target_link_libraries(urt_render
    PUBLIC
        urt::engine
        Vulkan::Vulkan
    PRIVATE
        urt_options)

# The shaders are committed already compiled (shaders/generated/*.h), so a
# normal build needs no shader compiler. After editing a .vert or .frag file,
# rebuild them with:  cmake --build --preset dev --target urt_shaders
find_program(URT_GLSLANG NAMES glslangValidator glslang
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
if(URT_GLSLANG)
    add_custom_target(urt_shaders
        COMMAND "${URT_GLSLANG}" -V shaders/scene.vert --vn scene_vert -o shaders/generated/scene_vert.h
        COMMAND "${URT_GLSLANG}" -V shaders/scene.frag --vn scene_frag -o shaders/generated/scene_frag.h
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Compiling shaders to SPIR-V"
        VERBATIM)
endif()
