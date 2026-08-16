cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

project(multi_index_lru
    VERSION 1.0.0
    DESCRIPTION "Header-only LRU container based on Boost.MultiIndex"
    HOMEPAGE_URL "https://github.com/mrayva/multi_index_lru"
    LANGUAGES CXX
)

# C++20 required
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Options
option(MULTI_INDEX_LRU_BUILD_TESTS "Build tests" ON)
option(MULTI_INDEX_LRU_BUILD_EXAMPLES "Build examples" ON)
option(MULTI_INDEX_LRU_BUILD_BENCHMARKS "Build performance benchmarks" OFF)
option(MULTI_INDEX_LRU_USE_BOOST_DEVELOP "Fetch a recent Boost.MultiIndex and its header dependencies" OFF)
option(MULTI_INDEX_LRU_BUILD_SBEPP_EXAMPLE "Build the sbepp integration example" OFF)
option(MULTI_INDEX_LRU_BUILD_ZERIALIZE_TESTS
    "Fetch the real zerialize library and test/exercise it directly, instead of only the mocks in zerialize_test.cpp/zerialize_cache.cpp" OFF)
option(MULTI_INDEX_LRU_BUILD_ZERIALIZE_BEVE_TESTS
    "Additionally build a separate, C++23-only test target covering zerialize's BEVE backend. Requires MULTI_INDEX_LRU_BUILD_ZERIALIZE_TESTS=ON; raises the required standard only for that one target, not the whole project" OFF)
set(MULTI_INDEX_LRU_BOOST_GIT_TAG "develop" CACHE STRING
    "Git tag or branch used for fetched Boost modules (for example develop or boost-1.91.0)")
set(MULTI_INDEX_LRU_ZERIALIZE_GIT_TAG "e67ed82c90c611db4c65aec946810409d962b3fc" CACHE STRING
    "Git tag/branch/commit of zerialize to fetch when MULTI_INDEX_LRU_BUILD_ZERIALIZE_TESTS is ON. \
Pinned to a specific commit rather than a floating branch (was \"main\" until this pin was \
introduced) so CI is reproducible and a breaking change upstream can't silently break this \
project's build with no corresponding commit here to bisect against -- override on the CMake \
command line if you need to track a different revision. e67ed82 fixes zerialize::translate() \
being O(n^2) in container size for maps/arrays.")

# Boost.MultiIndex handling
if(MULTI_INDEX_LRU_USE_BOOST_DEVELOP)
    message(STATUS "Fetching Boost.MultiIndex modules at ${MULTI_INDEX_LRU_BOOST_GIT_TAG}")
    
    include(FetchContent)
    
    # List of required Boost modules for multi_index
    set(BOOST_MODULES
        assert bind config container_hash core describe integer mp11 multi_index
        preprocessor smart_ptr throw_exception tuple type_traits utility
    )
    
    # Declare all modules
    set(BOOST_FETCHCONTENT_NAMES)
    foreach(module ${BOOST_MODULES})
        FetchContent_Declare(
            boost_${module}
            GIT_REPOSITORY https://github.com/boostorg/${module}.git
            GIT_TAG ${MULTI_INDEX_LRU_BOOST_GIT_TAG}
            GIT_SHALLOW TRUE
            SOURCE_SUBDIR multi_index_lru_headers_only
        )
        list(APPEND BOOST_FETCHCONTENT_NAMES boost_${module})
    endforeach()

    FetchContent_MakeAvailable(${BOOST_FETCHCONTENT_NAMES})

    # Create interface library for fetched Boost headers
    add_library(boost_multi_index_fetched INTERFACE)
    foreach(module ${BOOST_MODULES})
        target_include_directories(boost_multi_index_fetched INTERFACE
            ${boost_${module}_SOURCE_DIR}/include
        )
    endforeach()
    
    set(BOOST_TARGET boost_multi_index_fetched)
else()
    # Use system-installed Boost
    find_package(Boost 1.74 REQUIRED)
    set(BOOST_TARGET Boost::boost)
endif()

# Real zerialize (optional, opt-in)
#
# zerialize_test.cpp/zerialize_cache.cpp default to a hand-written
# mock_zerialize namespace so the library and its tests/examples build with
# zero external dependencies. That mock is useful for exercising this
# library's own logic (EntryBuilder, key extractors, Container integration)
# independently of any serialization format, but on its own it proves
# nothing about whether the real zerialize protocols actually work as
# Deserializer types here -- ZerializeFormatsTest's JSON/MsgPack/CBOR/Flex/
# ZERA/BSON/Ion cases, for example, all instantiate the exact same
# MockDeserializer under seven different aliased names. Turning this
# option on fetches the real library (which vendors its own protocol
# dependencies -- yyjson, msgpack-c, jsoncons, flatbuffers -- via its own
# CMakeLists.txt) and adds integration coverage against it; see
# test/zerialize_real_test.cpp and example/zerialize_real_cache.cpp.
#
# BEVE is covered by a separate MULTI_INDEX_LRU_BUILD_ZERIALIZE_BEVE_TESTS
# option (see test/zerialize_beve_test.cpp) rather than being folded into
# the above: it needs C++23 (glaze's own CMakeLists.txt hard-requires it).
# Enabling it turns on ZERIALIZE_ENABLE_BEVE for the one shared `zerialize`
# FetchContent target below, which makes THAT target itself require C++23
# -- and CMake computes a target's required standard from every target it
# links, so anything linking `zerialize` inherits that requirement too.
# test/zerialize_real_test.cpp lives in its own executable target
# specifically so it (and only it, plus the new zerialize_beve_test) is
# what moves to C++23 when this is turned on -- multi_index_lru_test never
# links `zerialize` at all, so it (and every base/compatibility-matrix CI
# job, none of which touch either zerialize option) stays C++20
# unconditionally. See test/CMakeLists.txt for the full explanation.
if(MULTI_INDEX_LRU_BUILD_ZERIALIZE_BEVE_TESTS AND NOT MULTI_INDEX_LRU_BUILD_ZERIALIZE_TESTS)
    message(FATAL_ERROR
        "MULTI_INDEX_LRU_BUILD_ZERIALIZE_BEVE_TESTS requires "
        "MULTI_INDEX_LRU_BUILD_ZERIALIZE_TESTS=ON too (BEVE's test target "
        "shares the same fetched zerialize dependency).")
endif()

if(MULTI_INDEX_LRU_BUILD_ZERIALIZE_TESTS)
    include(FetchContent)
    # Without this, zerialize's own CMakeLists.txt (via its CBOR/BSON
    # dependency) builds jsoncons' vendored unit-test suite too, which
    # currently fails under GCC with -Werror=maybe-uninitialized in one of
    # jsoncons' own test files (nothing to do with this project or
    # zerialize) -- confirmed by reproducing the failure with this line
    # removed. Same guard as nats_asio's CMakeLists.txt, which fetches the
    # same zerialize dependency.
    set(JSONCONS_BUILD_TESTS OFF CACHE BOOL "Disable jsoncons tests when vendored" FORCE)
    if(MULTI_INDEX_LRU_BUILD_ZERIALIZE_BEVE_TESTS)
        # Off by default in zerialize itself: pulling in glaze (BEVE's
        # backend) requires C++23, which would otherwise force that
        # standard on every consumer of the zerialize target. Only the
        # dedicated zerialize_beve_test executable (see test/CMakeLists.txt)
        # is actually compiled at C++23 -- this just makes the BEVE headers
        # available to fetch/link against.
        set(ZERIALIZE_ENABLE_BEVE ON CACHE BOOL "Enable BEVE (via glaze) for zerialize_beve_test" FORCE)
    endif()
    FetchContent_Declare(
        zerialize
        GIT_REPOSITORY https://github.com/mrayva/zerialize.git
        GIT_TAG ${MULTI_INDEX_LRU_ZERIALIZE_GIT_TAG}
    )
    FetchContent_MakeAvailable(zerialize)
endif()

# Header-only library target
add_library(multi_index_lru INTERFACE)
add_library(multi_index_lru::multi_index_lru ALIAS multi_index_lru)

target_include_directories(multi_index_lru INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(multi_index_lru INTERFACE
    ${BOOST_TARGET}
)

target_compile_features(multi_index_lru INTERFACE cxx_std_20)

# Tests
if(MULTI_INDEX_LRU_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

# Examples
if(MULTI_INDEX_LRU_BUILD_EXAMPLES)
    add_subdirectory(example)
endif()

if(MULTI_INDEX_LRU_BUILD_BENCHMARKS)
    add_subdirectory(benchmark)
endif()

# Installation (only when not using develop branch - it's not exportable)
if(NOT MULTI_INDEX_LRU_USE_BOOST_DEVELOP)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS multi_index_lru
        EXPORT multi_index_lru-targets
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    install(DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    install(FILES LICENSE NOTICE README.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR}
    )

    install(EXPORT multi_index_lru-targets
        FILE multi_index_lru-targets.cmake
        NAMESPACE multi_index_lru::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/multi_index_lru
    )

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/multi_index_lru-config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/multi_index_lru-config.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/multi_index_lru
    )

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/multi_index_lru-config-version.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/multi_index_lru-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/multi_index_lru-config-version.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/multi_index_lru
    )

    set(CPACK_PACKAGE_VENDOR "multi_index_lru contributors")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
    set(CPACK_GENERATOR "TGZ")
    include(CPack)
endif()
