cmake_minimum_required(VERSION 3.20)
project(ExchangeSequencer VERSION 0.1.0 LANGUAGES CXX)

# Require C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find Boost
# Boost.Interprocess is mostly header-only, but we find the Boost package globally here.
find_package(Boost 1.71 REQUIRED)

# --- Core Setup ---
# core is currently a header-only library
add_library(core_lib INTERFACE)
target_include_directories(core_lib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/core)
target_link_libraries(core_lib INTERFACE Boost::headers)

# --- Subdirectories ---
add_subdirectory(sequencer)

# Engine stub for later
# add_subdirectory(engine)
