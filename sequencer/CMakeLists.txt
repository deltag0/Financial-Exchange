# --- Sequencer Component ---

# Core sequencer library
add_library(sequencer_lib 
    lib/sequencer.hpp 
    core/sequencer.cpp
)
target_include_directories(sequencer_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/lib)
target_include_directories(sequencer_lib PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/core)

# Link the core exchange headers to the sequencer library
target_link_libraries(sequencer_lib PUBLIC core_lib)

# Sequencer Executable
add_executable(sequencer_app 
    core/main.cpp
)

# Connect the executable to its library
target_link_libraries(sequencer_app PRIVATE sequencer_lib)

# Boost Interprocess needs rt (Real Time) library on some Unix systems
if(UNIX AND NOT APPLE)
    target_link_libraries(sequencer_app PRIVATE rt)
endif()
