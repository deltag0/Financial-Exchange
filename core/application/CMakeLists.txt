add_library(application_event_json
    src/application_event_json.cpp
)

target_include_directories(application_event_json PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(application_event_json PUBLIC matching_engine)

add_library(application_event_gateway
    src/application_event_gateway.cpp
)

target_include_directories(application_event_gateway PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(application_event_gateway PUBLIC
    application_event_json
    recovered_run
)

add_library(https_static_server
    src/https_static_server.cpp
)

target_include_directories(https_static_server PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(https_static_server PRIVATE
    Boost::system
    OpenSSL::SSL
)

if(GTEST_FOUND)
    add_executable(application_event_json_tests
        tests/test_application_event_json.cpp
    )
    target_link_libraries(application_event_json_tests PRIVATE
        application_event_json
        GTest::gtest
        GTest::gtest_main
    )
    add_test(NAME ApplicationEventJsonUnitTests COMMAND application_event_json_tests)

    add_executable(application_event_gateway_tests
        tests/test_application_event_gateway.cpp
    )
    target_link_libraries(application_event_gateway_tests PRIVATE
        application_event_gateway
        GTest::gtest
        GTest::gtest_main
    )
    add_test(NAME ApplicationEventGatewayUnitTests COMMAND application_event_gateway_tests)

    add_executable(https_static_server_tests
        tests/test_https_static_server.cpp
    )
    target_link_libraries(https_static_server_tests PRIVATE
        https_static_server
        Boost::system
        OpenSSL::Crypto
        GTest::gtest
        GTest::gtest_main
    )
    add_test(NAME HttpsStaticServerUnitTests COMMAND https_static_server_tests)
endif()
