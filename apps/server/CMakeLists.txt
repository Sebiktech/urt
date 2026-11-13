add_executable(urt_server main.cpp)
target_link_libraries(urt_server PRIVATE urt::engine urt_options)
