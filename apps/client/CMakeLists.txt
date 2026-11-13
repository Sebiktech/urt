add_executable(urt_client main.cpp)
target_link_libraries(urt_client PRIVATE urt::engine urt_options)
