cmake_minimum_required(VERSION 3.20)
project(ExchangeSequencer VERSION 0.1.0 LANGUAGES CXX)

# Require C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find Boost
find_package(Boost 1.71 REQUIRED)

find_library(QUICKFIX_LIB quickfix REQUIRED)

# --- Core Setup ---
# core is a library that includes header-only components and FIX implementations
add_library(core_lib
    core/fix/src/fix_task.cpp
    core/fix/src/fix_parser.cpp
)
target_include_directories(core_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/core/shared_queue/include
    ${CMAKE_CURRENT_SOURCE_DIR}/core/fix/include
    ${CMAKE_CURRENT_SOURCE_DIR}/core/task/include
)
target_link_libraries(core_lib PUBLIC Boost::headers ${QUICKFIX_LIB})

# --- Subdirectories ---
add_subdirectory(sequencer)

# --- Exchange Executable ---
add_executable(sequencer_app 
    core/exchange/src/main.cpp
)
target_link_libraries(sequencer_app PRIVATE sequencer_lib)

# Boost Interprocess needs rt (Real Time) library on some Unix systems
if(UNIX AND NOT APPLE)
    target_link_libraries(sequencer_app PRIVATE rt)
endif()
