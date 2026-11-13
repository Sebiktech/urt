add_executable(urt_tests
    main.cpp
    test_application.cpp
    test_args.cpp
    test_input.cpp
    test_jobs.cpp
    test_log.cpp
    test_net_grid.cpp
    test_net_integration.cpp
    test_net_interpolation.cpp
    test_net_movement.cpp
    test_net_protocol.cpp
    test_script.cpp
    test_time.cpp)

target_link_libraries(urt_tests PRIVATE urt::engine doctest::doctest urt_options)

add_test(NAME urt_tests COMMAND urt_tests)

# End-to-end check: the real server binary runs a few ticks and exits cleanly.
add_test(NAME urt_server_smoke
    COMMAND urt_server --ticks 20 --tick-rate 100 --port 0 --log-level warn
            --resources "${CMAKE_SOURCE_DIR}/resources")
set_tests_properties(urt_tests urt_server_smoke PROPERTIES TIMEOUT 120)

if(URT_BUILD_RENDERER)
    # Draws a scene without a window and checks the picture. Needs a Vulkan
    # driver with a headless surface; otherwise it reports "skipped".
    add_executable(urt_render_headless render_headless.cpp)
    target_link_libraries(urt_render_headless PRIVATE urt::render urt_options)
    add_test(NAME urt_render_headless
        COMMAND urt_render_headless "${CMAKE_CURRENT_BINARY_DIR}/urt_render_headless.ppm")
    set_tests_properties(urt_render_headless PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

    # The same, but driven by a real server and client: checks what the
    # client would show in its window.
    add_executable(urt_client_render_headless client_render_headless.cpp)
    target_link_libraries(urt_client_render_headless PRIVATE urt_client_view urt_options)
    add_test(NAME urt_client_render_headless
        COMMAND urt_client_render_headless "${CMAKE_CURRENT_BINARY_DIR}/urt_client_render_headless.ppm")
    set_tests_properties(urt_client_render_headless PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
endif()
