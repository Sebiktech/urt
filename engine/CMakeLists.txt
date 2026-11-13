# urt_engine: everything shared by the server and the client.
# It must stay free of windowing and graphics code so the server can run on a
# machine with no GPU. The renderer will be a separate library that only the
# client links.

add_library(urt_engine STATIC
    src/app/application.cpp
    src/core/args.cpp
    src/core/jobs.cpp
    src/core/log.cpp
    src/core/platform.cpp
    src/core/time.cpp
    src/ecs/movement_system.cpp
    src/ecs/world.cpp)

add_library(urt::engine ALIAS urt_engine)

target_include_directories(urt_engine PUBLIC include)

target_compile_definitions(urt_engine PUBLIC URT_VERSION="${PROJECT_VERSION}")

target_link_libraries(urt_engine
    PUBLIC
        EnTT::EnTT
        glm::glm-header-only
        Threads::Threads
    PRIVATE
        urt_options)
