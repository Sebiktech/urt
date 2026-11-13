add_executable(urt_tests
    main.cpp
    test_application.cpp
    test_args.cpp
    test_input.cpp
    test_jobs.cpp
    test_log.cpp
    test_net_grid.cpp
    test_net_integration.cpp
    test_net_protocol.cpp
    test_time.cpp)

target_link_libraries(urt_tests PRIVATE urt::engine doctest::doctest urt_options)

add_test(NAME urt_tests COMMAND urt_tests)

# End-to-end check: the real server binary runs a few ticks and exits cleanly.
add_test(NAME urt_server_smoke
    COMMAND urt_server --ticks 20 --tick-rate 100 --npcs 5000 --port 0 --log-level warn)
set_tests_properties(urt_tests urt_server_smoke PROPERTIES TIMEOUT 120)
