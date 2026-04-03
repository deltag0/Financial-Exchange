# --- Sequencer Component ---

# Core sequencer library
add_library(sequencer_lib 
    include/sequencer.hpp 
    src/sequencer.cpp
)
target_include_directories(sequencer_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(sequencer_lib PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Link the core exchange headers to the sequencer library
target_link_libraries(sequencer_lib PUBLIC core_lib)

# Boost Interprocess needs rt (Real Time) library on some Unix systems
if(UNIX AND NOT APPLE)
    target_link_libraries(sequencer_lib PRIVATE rt)
endif()
