if(GTEST_FOUND)
    add_executable(command_admission_tests
        tests/test_command_admission.cpp
    )
    target_include_directories(command_admission_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/matching_engine/include
    )
    target_link_libraries(command_admission_tests PRIVATE core_lib GTest::gtest GTest::gtest_main)
    add_test(NAME CommandAdmissionUnitTests COMMAND command_admission_tests)

    add_executable(admission_completion_consumer_tests
        tests/test_admission_completion_consumer.cpp
    )
    target_include_directories(admission_completion_consumer_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/matching_engine/include
    )
    target_link_libraries(admission_completion_consumer_tests PRIVATE core_lib GTest::gtest GTest::gtest_main)
    add_test(NAME AdmissionCompletionConsumerUnitTests COMMAND admission_completion_consumer_tests)
endif()
