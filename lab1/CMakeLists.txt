cmake_minimum_required(VERSION 3.25)
project(lab1 CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(lab1
        src/main.cpp
        src/daemon/daemon.cpp
        src/daemon/daemon.hpp
        src/config/config.cpp
        src/config/config.hpp
        src/logger/logger.cpp
        src/logger/logger.hpp
        src/util/util.cpp
        src/util/util.hpp
        )

# Warnings
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(lab1 PRIVATE -Wall -Werror)
endif()

# pthread (std::thread)
find_package(Threads REQUIRED)
target_link_libraries(lab1 PRIVATE Threads::Threads)

# Output dir
set_target_properties(lab1 PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
        )