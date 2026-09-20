add_library(storage_codec
    src/crc32c.cpp
    src/exchange_run_id_reservation.cpp
    src/journal_command_codec.cpp
    src/journal_run_header_codec.cpp
    src/new_run_activation.cpp
    src/new_run_journal.cpp
    src/run_catalog_codec.cpp
    src/run_catalog_storage.cpp
    src/run_journal_reader.cpp
    src/run_journal_recovery.cpp
    src/run_journal_storage.cpp
    src/run_journal_writer.cpp
)
target_include_directories(storage_codec PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/core/domain/include
    ${CMAKE_SOURCE_DIR}/core/task/include
)

if(GTEST_FOUND)
    add_executable(storage_codec_tests
        tests/test_exchange_run_id_reservation.cpp
        tests/test_journal_command_codec.cpp
        tests/test_journal_run_header_codec.cpp
        tests/test_new_run_activation.cpp
        tests/test_new_run_journal.cpp
        tests/test_run_catalog_codec.cpp
        tests/test_run_catalog_storage.cpp
        tests/test_run_journal_reader.cpp
        tests/test_run_journal_recovery.cpp
        tests/test_run_journal_storage.cpp
        tests/test_run_journal_writer.cpp
    )
    target_link_libraries(storage_codec_tests PRIVATE storage_codec GTest::gtest GTest::gtest_main)
    target_include_directories(storage_codec_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME StorageCodecUnitTests COMMAND storage_codec_tests)
endif()
