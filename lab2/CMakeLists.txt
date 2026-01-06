cmake_minimum_required(VERSION 3.25)
project(test)

set(CMAKE_CXX_STANDARD 17)

add_executable(host
        src/host.cpp
        )

add_executable(client
        src/client.cpp
        )