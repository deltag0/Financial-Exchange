add_library(recovered_run
    src/recovered_run.cpp
    src/start_new_run.cpp
    src/exchange_run_controller.cpp
    src/controller_command_processing.cpp
)

target_include_directories(recovered_run PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_include_directories(recovered_run PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../storage/src)

target_link_libraries(recovered_run PUBLIC
    matching_engine
    storage_codec
    PRIVATE sequencer_lib
)

if(GTEST_FOUND)
    add_executable(recovered_run_tests
        tests/test_recovered_run.cpp
        tests/test_start_new_run.cpp
        tests/test_exchange_run_controller.cpp
        tests/test_controller_command_processing.cpp
    )
    target_include_directories(recovered_run_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/../storage/src
    )
    target_link_libraries(recovered_run_tests PRIVATE recovered_run GTest::gtest GTest::gtest_main)
    add_test(NAME RecoveredRunUnitTests COMMAND recovered_run_tests)
endif()
