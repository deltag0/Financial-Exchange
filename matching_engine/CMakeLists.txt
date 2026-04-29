# Matching Engine Library
add_library(matching_engine INTERFACE)

target_include_directories(matching_engine INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(matching_engine INTERFACE core)
