# Experimental POC: multi_index_lru served over an iceoryx2 request-response
# service. NOT part of the main multi_index_lru build (see MULTI_INDEX_LRU_
# BUILD_EXAMPLES in the top-level CMakeLists.txt) because it pulls in a Rust
# toolchain via iceoryx2-cxx, which the rest of this project intentionally
# does not depend on.
#
# Build iceoryx2-cxx first and point CMAKE_PREFIX_PATH at its install prefix,
# see README.md in this directory for the exact steps. Then:
#
#   cmake -S example/iceoryx2_cache_poc -B build-poc \
#         -DCMAKE_PREFIX_PATH=/path/to/iceoryx2/install
#   cmake --build build-poc
#
# POC_ENABLE_NATS_READTHROUGH additionally builds server_readthrough.cpp /
# client_readthrough.cpp, which pull in nats_asio (and its own, much heavier
# dependency graph: asio, fmt, concurrentqueue, gtl, magic-enum, nlohmann-
# json, openssl, simdjson, spdlog, stringzilla, zstd -- see README.md
# "Setup"). Off by default so the plain iceoryx2 demo stays buildable with
# only iceoryx2-cxx.
cmake_minimum_required(VERSION 3.22)
project(multi_index_lru_iceoryx2_poc LANGUAGES CXX)

option(POC_ENABLE_NATS_READTHROUGH "Also build the NATS-backed read-through/write-through server and client" OFF)
option(POC_BUILD_TESTS "Build the ctest-integrated wire.hpp unit tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(iceoryx2-cxx 0.9.999 REQUIRED)
find_package(Boost 1.74 REQUIRED)

add_library(multi_index_lru_headers INTERFACE)
target_include_directories(multi_index_lru_headers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)
target_link_libraries(multi_index_lru_headers INTERFACE Boost::boost)

add_executable(cache_poc_server src/server.cpp)
target_link_libraries(cache_poc_server
    PRIVATE multi_index_lru_headers iceoryx2-cxx::static-lib-cxx
)

add_executable(cache_poc_client src/client.cpp)
target_link_libraries(cache_poc_client
    PRIVATE multi_index_lru_headers iceoryx2-cxx::static-lib-cxx
)

if(POC_ENABLE_NATS_READTHROUGH)
    find_package(nats_asio CONFIG REQUIRED)
    find_package(Threads REQUIRED)

    add_executable(cache_poc_server_readthrough src/server_readthrough.cpp)
    target_link_libraries(cache_poc_server_readthrough
        PRIVATE multi_index_lru_headers iceoryx2-cxx::static-lib-cxx nats_asio::nats_asio Threads::Threads
    )

    add_executable(cache_poc_client_readthrough src/client_readthrough.cpp)
    target_link_libraries(cache_poc_client_readthrough
        PRIVATE multi_index_lru_headers iceoryx2-cxx::static-lib-cxx nats_asio::nats_asio Threads::Threads
    )
endif()

# wire.hpp has no dependency on iceoryx2/nats_asio/Boost/multi_index_lru --
# it's a standalone little-endian encoder/decoder -- so its test target only
# links against GTest, even though (like everything else in this directory)
# actually configuring this project still requires iceoryx2-cxx to be found
# above.
if(POC_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.17.0
        GIT_SHALLOW TRUE
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()

    add_executable(cache_poc_wire_test test/wire_test.cpp)
    target_link_libraries(cache_poc_wire_test PRIVATE GTest::gtest GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(cache_poc_wire_test)
endif()
