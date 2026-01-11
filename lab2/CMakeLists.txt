cmake_minimum_required(VERSION 3.16)
project(os_lab2_v10 CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_compile_options(-O2 -pipe -Wall -Werror)

find_package(Threads REQUIRED)
find_library(LRT rt)

file(GLOB_RECURSE LAB2_COMMON_CPP CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/common/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/transport/*.cpp"
        )

add_library(lab2_common STATIC ${LAB2_COMMON_CPP})
target_include_directories(lab2_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(lab2_common PUBLIC Threads::Threads)
if (LRT)
    target_link_libraries(lab2_common PUBLIC ${LRT})
endif ()


file(GLOB_RECURSE LAB2_SOCK_CPP CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/connections/sock/*.cpp"
        )

add_library(lab2_sock STATIC ${LAB2_SOCK_CPP})
target_include_directories(lab2_sock PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(lab2_sock PUBLIC lab2_common)

add_executable(host_sock "${CMAKE_CURRENT_SOURCE_DIR}/src/host.cpp")
target_include_directories(host_sock PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(host_sock PRIVATE lab2_sock)

add_executable(client_sock "${CMAKE_CURRENT_SOURCE_DIR}/src/client.cpp")
target_include_directories(client_sock PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(client_sock PRIVATE lab2_sock)


if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/connections/fifo/transport_fifo.cpp")
    file(GLOB_RECURSE LAB2_FIFO_CPP CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/src/connections/fifo/*.cpp"
            )
    add_library(lab2_fifo STATIC ${LAB2_FIFO_CPP})
    target_include_directories(lab2_fifo PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(lab2_fifo PUBLIC lab2_common)

    add_executable(host_fifo "${CMAKE_CURRENT_SOURCE_DIR}/src/host.cpp")
    target_include_directories(host_fifo PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(host_fifo PRIVATE lab2_fifo)

    add_executable(client_fifo "${CMAKE_CURRENT_SOURCE_DIR}/src/client.cpp")
    target_include_directories(client_fifo PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(client_fifo PRIVATE lab2_fifo)
endif ()


if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/connections/mq/transport_mq.cpp")
    file(GLOB_RECURSE LAB2_MQ_CPP CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/src/connections/mq/*.cpp"
            )
    add_library(lab2_mq STATIC ${LAB2_MQ_CPP})
    target_include_directories(lab2_mq PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(lab2_mq PUBLIC lab2_common)

    add_executable(host_mq "${CMAKE_CURRENT_SOURCE_DIR}/src/host.cpp")
    target_include_directories(host_mq PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(host_mq PRIVATE lab2_mq)

    add_executable(client_mq "${CMAKE_CURRENT_SOURCE_DIR}/src/client.cpp")
    target_include_directories(client_mq PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(client_mq PRIVATE lab2_mq)
endif ()
