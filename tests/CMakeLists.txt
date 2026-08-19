cmake_minimum_required(VERSION 3.16)
project(klima_protocol_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(
    protocol_host_test
    protocol_host_test.c
    ../main/ac_protocol.c
)
target_include_directories(protocol_host_test PRIVATE ../main)
if(MSVC)
    target_compile_options(protocol_host_test PRIVATE /W4 /WX)
else()
    target_compile_options(protocol_host_test PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

enable_testing()
add_test(NAME protocol_host_test COMMAND protocol_host_test)
