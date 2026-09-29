if (BUILD_TESTING)
    function(define_server_api_tests)
        set(MOD_PATH "${CMAKE_SOURCE_DIR}/modules/mod-server-api")

        add_executable(server_api_tests
            "${MOD_PATH}/tests/TestEventBus.cpp"
            "${MOD_PATH}/tests/TestHttpUtils.cpp"
            "${MOD_PATH}/tests/TestRequestRateLimiter.cpp"
            "${MOD_PATH}/tests/TestModuleRegistry.cpp"
            "${MOD_PATH}/tests/TestCommandQueue.cpp"
        )

        target_link_libraries(server_api_tests
            game
            gtest_main
            modules
            scripts
            game-interface
        )

        target_include_directories(server_api_tests PRIVATE
            "${MOD_PATH}/src"
        )

        set_target_properties(server_api_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        )

        add_test(NAME server_api_tests COMMAND server_api_tests)
    endfunction()

    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL define_server_api_tests)
endif()
