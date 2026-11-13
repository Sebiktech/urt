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
    src/ecs/world.cpp
    src/net/bytes.cpp
    src/net/client.cpp
    src/net/interest_grid.cpp
    src/net/interpolation.cpp
    src/net/protocol.cpp
    src/net/server.cpp
    src/net/transport.cpp
    src/script/client_script_system.cpp
    src/script/host.cpp
    src/script/script_system.cpp
    src/script/value_codec.cpp
    src/script/vm.cpp)

add_library(urt::engine ALIAS urt_engine)

target_include_directories(urt_engine PUBLIC include)

target_compile_definitions(urt_engine PUBLIC URT_VERSION="${PROJECT_VERSION}")

target_link_libraries(urt_engine
    PUBLIC
        EnTT::EnTT
        glm::glm-header-only
        Threads::Threads
    PRIVATE
        enet
        Luau.Compiler
        Luau.VM
        urt_options)
