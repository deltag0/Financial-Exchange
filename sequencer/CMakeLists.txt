# --- Sequencer Component ---

# Core sequencer library
add_library(sequencer_lib 
    include/sequencer.hpp 
    src/sequencer.cpp
)
target_include_directories(sequencer_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(sequencer_lib PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Link the core exchange headers to the sequencer library
target_link_libraries(sequencer_lib PUBLIC core_lib storage_codec)

add_executable(sequencer_production_loop_smoke
    tests/test_production_loop_smoke.cpp
)
target_include_directories(sequencer_production_loop_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ..)
target_link_libraries(sequencer_production_loop_smoke PRIVATE sequencer_lib)
add_test(NAME SequencerProductionLoopSmokeTest COMMAND sequencer_production_loop_smoke)
set_tests_properties(SequencerProductionLoopSmokeTest PROPERTIES TIMEOUT 5)

# Boost Interprocess needs rt (Real Time) library on some Unix systems
if(UNIX AND NOT APPLE)
    target_link_libraries(sequencer_lib PRIVATE rt)
endif()

# Sequencer tests
if(GTEST_FOUND)
    add_executable(sequencer_journal_tests tests/test_journal_sequencing.cpp)
    target_include_directories(sequencer_journal_tests PRIVATE ${CMAKE_SOURCE_DIR}/core/storage/src)
    target_link_libraries(sequencer_journal_tests PRIVATE sequencer_lib recovered_run GTest::gtest GTest::gtest_main)
    add_test(NAME SequencerJournalUnitTests COMMAND sequencer_journal_tests)

    add_executable(e2e_throughput_tests
        tests/test_e2e_throughput.cpp
    )
    target_include_directories(e2e_throughput_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ..)
    target_link_libraries(e2e_throughput_tests PRIVATE sequencer_lib matching_engine core_lib GTest::gtest GTest::gtest_main)
    add_test(NAME E2EThroughputTests COMMAND e2e_throughput_tests)
endif()
