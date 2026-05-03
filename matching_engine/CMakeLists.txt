# Matching Engine Library
add_library(matching_engine
    src/matching_engine.cpp
)

target_include_directories(matching_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(matching_engine PUBLIC core_lib)

if(GTEST_FOUND)
    add_executable(matching_engine_tests
        tests/test_matching_engine.cpp
    )
    target_include_directories(matching_engine_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ..)
    target_link_libraries(matching_engine_tests PRIVATE matching_engine core_lib GTest::gtest GTest::gtest_main)
    add_test(NAME MatchingEngineUnitTests COMMAND matching_engine_tests)
endif()
