cmake_minimum_required(VERSION 3.16)
project(os_lab2_v10 CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_compile_options(-O2 -pipe -pthread -Wall -Werror)

# Общие исходники
set(SRC_COMMON
        src/common.cpp
        src/conn_mq.cpp
        src/conn_fifo.cpp
        src/conn_sock.cpp
        )

add_library(core_objs STATIC ${SRC_COMMON})
target_include_directories(core_objs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# Для mq в glibc иногда нужен -lrt
find_library(LRT rt)
if(LRT)
    target_link_libraries(core_objs PUBLIC ${LRT})
endif()
target_link_libraries(core_objs PUBLIC pthread)

# Хост и клиент (один код с разными флагами трансп./линковкой)
add_executable(host_mq src/host.cpp)
target_link_libraries(host_mq PRIVATE core_objs)
target_compile_definitions(host_mq PRIVATE TRANSPORT_MQ)

add_executable(client_mq src/client.cpp)
target_link_libraries(client_mq PRIVATE core_objs)
target_compile_definitions(client_mq PRIVATE TRANSPORT_MQ)

add_executable(host_fifo src/host.cpp)
target_link_libraries(host_fifo PRIVATE core_objs)
target_compile_definitions(host_fifo PRIVATE TRANSPORT_FIFO)

add_executable(client_fifo src/client.cpp)
target_link_libraries(client_fifo PRIVATE core_objs)
target_compile_definitions(client_fifo PRIVATE TRANSPORT_FIFO)

add_executable(host_sock src/host.cpp)
target_link_libraries(host_sock PRIVATE core_objs)
target_compile_definitions(host_sock PRIVATE TRANSPORT_SOCK)

add_executable(client_sock src/client.cpp)
target_link_libraries(client_sock PRIVATE core_objs)
target_compile_definitions(client_sock PRIVATE TRANSPORT_SOCK)
