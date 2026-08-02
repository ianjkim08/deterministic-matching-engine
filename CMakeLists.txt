cmake_minimum_required(VERSION 3.20)
project(deterministic_matching_engine VERSION 1.0.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)
find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(DME_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

function(dme_enable_warnings target)
    target_compile_options(${target} PRIVATE
        # C4324 reports the intentional cache-line padding around SPSC indices.
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /wd4324>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Wconversion>
    )
    if(DME_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE
            $<$<CXX_COMPILER_ID:MSVC>:/WX>
            $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Werror>
        )
    endif()
endfunction()

add_library(dme
    src/order_book.cpp
    src/journal.cpp
    src/protocol.cpp
    src/gateway.cpp
)
target_include_directories(dme PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
dme_enable_warnings(dme)

add_executable(dme_bench apps/benchmark.cpp)
target_link_libraries(dme_bench PRIVATE dme Threads::Threads)
dme_enable_warnings(dme_bench)

add_executable(dme_replay apps/replay.cpp)
target_link_libraries(dme_replay PRIVATE dme)
dme_enable_warnings(dme_replay)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(dme_gateway apps/gateway.cpp)
    target_link_libraries(dme_gateway PRIVATE dme Threads::Threads)
    dme_enable_warnings(dme_gateway)
endif()

if(BUILD_TESTING)
    add_executable(dme_tests tests/test_engine.cpp)
    target_link_libraries(dme_tests PRIVATE dme Threads::Threads)
    dme_enable_warnings(dme_tests)
    add_test(NAME dme_tests COMMAND dme_tests)
    add_executable(dme_differential_tests tests/test_differential.cpp)
    target_link_libraries(dme_differential_tests PRIVATE dme Threads::Threads)
    dme_enable_warnings(dme_differential_tests)
    add_test(NAME dme_differential_tests COMMAND dme_differential_tests)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(dme_gateway_socket_tests tests/test_gateway_socket.cpp)
        target_link_libraries(dme_gateway_socket_tests PRIVATE dme Threads::Threads)
        dme_enable_warnings(dme_gateway_socket_tests)
        add_test(NAME dme_gateway_socket_tests COMMAND dme_gateway_socket_tests)
    endif()
endif()

install(TARGETS dme dme_bench dme_replay
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
if(TARGET dme_gateway)
    install(TARGETS dme_gateway RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
