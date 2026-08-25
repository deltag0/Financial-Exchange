if(GTEST_FOUND)
    add_executable(fix_client_identity_tests
        tests/test_client_identity.cpp
    )
    target_include_directories(fix_client_identity_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_SOURCE_DIR}/../../sequencer/include ${CMAKE_CURRENT_SOURCE_DIR}/../shared_queue/include ${CMAKE_CURRENT_SOURCE_DIR}/../bus/include ${CMAKE_CURRENT_SOURCE_DIR}/../task/include)
    target_compile_definitions(fix_client_identity_tests PRIVATE EXCHANGE_CONFIG_PATH="${CMAKE_SOURCE_DIR}/exchange.cfg")
    target_link_libraries(fix_client_identity_tests PRIVATE core_lib GTest::gtest GTest::gtest_main ${QUICKFIX_LIB})
    add_test(NAME FixClientIdentityUnitTests COMMAND fix_client_identity_tests)

    add_executable(fix_parser_tests
        tests/test_fix_parser.cpp
    )
    target_sources(fix_parser_tests PRIVATE tests/test_fix_parser_edgecases.cpp)
    target_sources(fix_parser_tests PRIVATE tests/test_fix_malformed.cpp)
    target_include_directories(fix_parser_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_SOURCE_DIR}/../../sequencer/include ${CMAKE_CURRENT_SOURCE_DIR}/../shared_queue/include ${CMAKE_CURRENT_SOURCE_DIR}/../bus/include ${CMAKE_CURRENT_SOURCE_DIR}/../task/include)
    target_link_libraries(fix_parser_tests PRIVATE core_lib GTest::gtest GTest::gtest_main ${QUICKFIX_LIB})
    add_test(NAME FixParserUnitTests COMMAND fix_parser_tests)

    add_executable(fix_task_tests
        tests/test_fix_task.cpp
    )
    target_sources(fix_task_tests PRIVATE tests/test_fix_parser_edgecases.cpp)
    target_sources(fix_task_tests PRIVATE tests/test_fix_malformed.cpp)
    target_include_directories(fix_task_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_SOURCE_DIR}/../../sequencer/include ${CMAKE_CURRENT_SOURCE_DIR}/../shared_queue/include ${CMAKE_CURRENT_SOURCE_DIR}/../bus/include ${CMAKE_CURRENT_SOURCE_DIR}/../task/include)
    target_link_libraries(fix_task_tests PRIVATE core_lib GTest::gtest GTest::gtest_main ${QUICKFIX_LIB})
    add_test(NAME FixTaskUnitTests COMMAND fix_task_tests)
endif()
