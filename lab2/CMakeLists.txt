cmake_minimum_required(VERSION 3.16)
project(os_lab2_v10 CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_compile_options(-O2 -pipe -pthread -Wall -Werror)

# Common sources (all *.cpp except host/client and conn_*.cpp).
file(GLOB SRC_ALL "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
set(HOST_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/host.cpp")
set(CLIENT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/client.cpp")
file(GLOB CONN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/conn_*.cpp")

set(COMMON_SOURCES ${SRC_ALL})
list(REMOVE_ITEM COMMON_SOURCES ${HOST_SRC} ${CLIENT_SRC})
foreach(f ${CONN_SOURCES})
  list(REMOVE_ITEM COMMON_SOURCES ${f})
endforeach()

find_library(LRT rt)

# For each transport implementation src/conn_<type>.cpp generate:
#   - static lib core_<type>
#   - host_<type>
#   - client_<type>
foreach(conn_file ${CONN_SOURCES})
  get_filename_component(conn_we "${conn_file}" NAME_WE)   # conn_mq
  string(REPLACE "conn_" "" type_code "${conn_we}")      # mq
  string(TOUPPER "${type_code}" TYPE_UPPER)

  set(core_lib "core_${type_code}")
  add_library(${core_lib} STATIC ${COMMON_SOURCES} ${conn_file})
  target_include_directories(${core_lib} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(${core_lib} PUBLIC pthread)
  if(LRT)
    target_link_libraries(${core_lib} PUBLIC ${LRT})
  endif()

  add_executable(host_${type_code} "${HOST_SRC}")
  target_link_libraries(host_${type_code} PRIVATE ${core_lib})
  target_compile_definitions(host_${type_code} PRIVATE "TRANSPORT_${TYPE_UPPER}")

  add_executable(client_${type_code} "${CLIENT_SRC}")
  target_link_libraries(client_${type_code} PRIVATE ${core_lib})
  target_compile_definitions(client_${type_code} PRIVATE "TRANSPORT_${TYPE_UPPER}")
endforeach()
