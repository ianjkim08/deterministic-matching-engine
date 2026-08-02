cmake_minimum_required(VERSION 3.20)
project(deterministic_matching_engine VERSION 1.0.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)
find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(dme
    src/order_book.cpp
    src/journal.cpp
)
target_include_directories(dme PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_options(dme PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Wconversion>
)

add_executable(dme_bench apps/benchmark.cpp)
target_link_libraries(dme_bench PRIVATE dme Threads::Threads)

add_executable(dme_replay apps/replay.cpp)
target_link_libraries(dme_replay PRIVATE dme)

if(BUILD_TESTING)
    add_executable(dme_tests tests/test_engine.cpp)
    target_link_libraries(dme_tests PRIVATE dme Threads::Threads)
    add_test(NAME dme_tests COMMAND dme_tests)
endif()

install(TARGETS dme dme_bench dme_replay
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
