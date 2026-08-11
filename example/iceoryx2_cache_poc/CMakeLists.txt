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
#
# POC_BUILD_ICEORYX2_TARGETS (default ON) gates iceoryx2-cxx itself and every
# target that links it. Turned OFF -- e.g. `-DPOC_BUILD_ICEORYX2_TARGETS=OFF
# -DPOC_BUILD_TESTS=ON` -- this directory configures and builds with nothing
# but Boost and a C++20 compiler, no Rust toolchain required: wire.hpp and
# handle_request_local() (cache_service.hpp) have no dependency on iceoryx2
# or nats_asio, so their unit tests don't need either. That's what lets the
# main CI workflow test them without taking on the Rust/iceoryx2-cxx build.
cmake_minimum_required(VERSION 3.22)
project(multi_index_lru_iceoryx2_poc LANGUAGES CXX)

option(POC_BUILD_ICEORYX2_TARGETS "Build the iceoryx2-backed servers/clients (requires iceoryx2-cxx)" ON)
option(POC_ENABLE_NATS_READTHROUGH "Also build the NATS-backed read-through/write-through server and client" OFF)
option(POC_ENABLE_ECAL_READTHROUGH "Also build the eCAL-backed read-through/write-through server and client (requires eCAL, no iceoryx2-cxx/Rust needed)" OFF)
option(POC_BUILD_TESTS "Build the ctest-integrated unit tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Boost 1.74 REQUIRED)

add_library(multi_index_lru_headers INTERFACE)
target_include_directories(multi_index_lru_headers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)
target_link_libraries(multi_index_lru_headers INTERFACE Boost::boost)

if(POC_BUILD_ICEORYX2_TARGETS)
    find_package(iceoryx2-cxx 0.9.999 REQUIRED)

    add_executable(cache_poc_server src/server.cpp)
    target_link_libraries(cache_poc_server
        PRIVATE multi_index_lru_headers iceoryx2-cxx::static-lib-cxx
    )

    add_executable(cache_poc_client src/client.cpp)
    target_link_libraries(cache_poc_client
        PRIVATE multi_index_lru_headers iceoryx2-cxx::static-lib-cxx
    )

    # A load-test client, not a server: no nats_asio dependency of its own,
    # so it builds even with POC_ENABLE_NATS_READTHROUGH=OFF -- it just
    # wouldn't have anything interesting (NATS-backed) to point at in that
    # case. See src/load_test.cpp and README.md "Load testing".
    add_executable(cache_poc_load_test src/load_test.cpp)
    target_link_libraries(cache_poc_load_test
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
endif()

# eCAL port of the NATS read-through/write-through server+client -- same
# dispatch_request()/CompletionQueue/KeyOperationQueue logic
# (server_dispatch_common.hpp), served over an eCAL Service instead of an
# iceoryx2 request-response service. See active_request_ecal.hpp's doc
# comment for the architectural difference this forces (eCAL's method
# callback is synchronous, unlike iceoryx2's defer-and-respond-later
# ActiveRequest) and server_readthrough_ecal.cpp for how it's handled.
# Deliberately independent of POC_BUILD_ICEORYX2_TARGETS -- eCAL is a fully
# separate transport, so this needs no Rust toolchain at all.
if(POC_ENABLE_ECAL_READTHROUGH)
    find_package(eCAL REQUIRED)
    find_package(nats_asio CONFIG REQUIRED)
    find_package(Threads REQUIRED)

    add_executable(cache_poc_server_readthrough_ecal src/server_readthrough_ecal.cpp)
    target_compile_definitions(cache_poc_server_readthrough_ecal PRIVATE POC_USE_ECAL)
    target_link_libraries(cache_poc_server_readthrough_ecal
        PRIVATE multi_index_lru_headers eCAL::core nats_asio::nats_asio Threads::Threads
    )

    add_executable(cache_poc_client_readthrough_ecal src/client_readthrough_ecal.cpp)
    target_compile_definitions(cache_poc_client_readthrough_ecal PRIVATE POC_USE_ECAL)
    target_link_libraries(cache_poc_client_readthrough_ecal
        PRIVATE multi_index_lru_headers eCAL::core nats_asio::nats_asio Threads::Threads
    )
endif()

# wire.hpp has no dependency on iceoryx2/nats_asio/Boost/multi_index_lru --
# it's a standalone little-endian encoder/decoder -- so its test target only
# links against GTest. handle_request_local() (cache_service.hpp) needs
# multi_index_lru + Boost for NameCache/IdCache but still nothing from
# iceoryx2 or nats_asio, so it links multi_index_lru_headers instead. Both
# build with POC_BUILD_ICEORYX2_TARGETS=OFF.
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

    add_executable(cache_poc_handle_request_local_test test/handle_request_local_test.cpp)
    target_link_libraries(cache_poc_handle_request_local_test
        PRIVATE multi_index_lru_headers GTest::gtest GTest::gtest_main
    )

    # config.hpp is standard-library-only, same as wire.hpp.
    add_executable(cache_poc_config_test test/config_test.cpp)
    target_link_libraries(cache_poc_config_test PRIVATE GTest::gtest GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(cache_poc_wire_test)
    gtest_discover_tests(cache_poc_handle_request_local_test)
    gtest_discover_tests(cache_poc_config_test)

    # nats_bridge_test.cpp and dispatch_request_test.cpp are integration
    # tests against a real local NATS server (there's no fake for
    # nats_asio::iconnection, and no way to construct a standalone iceoryx2
    # ActiveRequest) -- see their file comments for the one-time `nats kv
    # add` setup they need. Built only when POC_ENABLE_NATS_READTHROUGH
    # already pulled in nats_asio; not part of the dependency-free
    # poc-unit-tests CI job for that reason.
    if(POC_ENABLE_NATS_READTHROUGH)
        add_executable(cache_poc_nats_bridge_test test/nats_bridge_test.cpp)
        target_link_libraries(cache_poc_nats_bridge_test
            PRIVATE multi_index_lru_headers nats_asio::nats_asio Threads::Threads GTest::gtest GTest::gtest_main
        )

        add_executable(cache_poc_dispatch_request_test test/dispatch_request_test.cpp)
        target_link_libraries(cache_poc_dispatch_request_test
            PRIVATE multi_index_lru_headers iceoryx2-cxx::static-lib-cxx nats_asio::nats_asio Threads::Threads
                    GTest::gtest GTest::gtest_main
        )

        # Worked example (attribute_cache.hpp/nats_key.hpp) verified against
        # real NATS -- same reason as the two above for why this needs
        # POC_ENABLE_NATS_READTHROUGH and isn't part of poc-unit-tests CI.
        add_executable(cache_poc_composite_key_nats_test test/composite_key_nats_test.cpp)
        target_link_libraries(cache_poc_composite_key_nats_test
            PRIVATE multi_index_lru_headers nats_asio::nats_asio Threads::Threads GTest::gtest GTest::gtest_main
        )

        # Worked example (security_cache.hpp/nats_key.hpp) for
        # NatsCompositeKey::pattern() -- wildcarding an interior field
        # position, not just prefix_pattern()'s trailing run. Same reason
        # as the one above for why this needs POC_ENABLE_NATS_READTHROUGH
        # and isn't part of poc-unit-tests CI.
        add_executable(cache_poc_security_key_nats_test test/security_key_nats_test.cpp)
        target_link_libraries(cache_poc_security_key_nats_test
            PRIVATE multi_index_lru_headers nats_asio::nats_asio Threads::Threads GTest::gtest GTest::gtest_main
        )

        gtest_discover_tests(cache_poc_nats_bridge_test)
        gtest_discover_tests(cache_poc_dispatch_request_test)
        gtest_discover_tests(cache_poc_composite_key_nats_test)
        gtest_discover_tests(cache_poc_security_key_nats_test)
    endif()
endif()
