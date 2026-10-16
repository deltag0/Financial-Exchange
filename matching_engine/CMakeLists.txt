# Matching Engine Library
add_library(matching_engine
    src/matching_engine.cpp
    src/private_result_projection.cpp
    src/public_trade_projection.cpp
    src/run_journal_replay.cpp
)

target_include_directories(matching_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(matching_engine PUBLIC core_lib PRIVATE storage_codec)

if(GTEST_FOUND)
    add_executable(matching_engine_tests
        ../core/admission/tests/test_command_admission_recovery.cpp
        tests/test_matching_engine.cpp
        tests/test_private_result_projection.cpp
        tests/test_private_result_queue.cpp
        tests/test_public_trade_queue.cpp
        tests/test_public_trade_projection.cpp
        tests/test_run_journal_replay.cpp
    )
    target_include_directories(matching_engine_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ..)
    target_link_libraries(matching_engine_tests PRIVATE matching_engine core_lib GTest::gtest GTest::gtest_main)
    add_test(NAME MatchingEngineUnitTests COMMAND matching_engine_tests)
endif()
