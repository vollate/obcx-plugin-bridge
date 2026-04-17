# Telegram to QQ Plugin CMakeLists.txt

# Create the TG to QQ plugin shared library
add_library(tg_to_qq_plugin SHARED tg_to_qq_plugin.cpp tg_to_qq_plugin.hpp)

# Set the output name to match plugin loading expectations
set_target_properties(
  tg_to_qq_plugin
  PROPERTIES OUTPUT_NAME "tg_to_qq"
             PREFIX "" # No lib prefix
             POSITION_INDEPENDENT_CODE ON
             CXX_STANDARD 26
             CXX_STANDARD_REQUIRED ON)

# Include directories
target_include_directories(
  tg_to_qq_plugin PRIVATE ${CMAKE_SOURCE_DIR}/examples
                          ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# Link against OBCX core and dependency/bridge_bot dependencies
target_link_libraries(tg_to_qq_plugin PRIVATE bridge_core)

# Set the output directory to build/plugins
set_target_properties(tg_to_qq_plugin PROPERTIES LIBRARY_OUTPUT_DIRECTORY
                                                 ${CMAKE_BINARY_DIR}/plugins)
