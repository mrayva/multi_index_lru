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

add_executable(multi_index_lru_test
    container_test.cpp
    zerialize_test.cpp
    expirable_test.cpp
    sbe_test.cpp
    nats_key_test.cpp
)

target_link_libraries(multi_index_lru_test PRIVATE
    multi_index_lru::multi_index_lru
    GTest::gtest
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(multi_index_lru_test)

# zerialize_real_test.cpp deliberately lives in its own executable, not as
# extra sources on multi_index_lru_test above: once
# MULTI_INDEX_LRU_BUILD_ZERIALIZE_BEVE_TESTS also enables
# ZERIALIZE_ENABLE_BEVE (see the top-level CMakeLists.txt), the *shared*
# `zerialize` INTERFACE target itself starts requiring C++23 (it links
# glaze), and that requirement propagates to every target that links
# `zerialize` -- CMake computes a target's required standard from the
# union of its own sources and everything it links, not per translation
# unit. If multi_index_lru_test linked `zerialize` directly, turning BEVE
# on would silently raise container_test.cpp/expirable_test.cpp/etc.'s
# required standard too, even though they have nothing to do with
# zerialize -- confirmed by observing exactly that leak (all of
# multi_index_lru_test's sources compiling at -std=c++23) before this was
# split out. Keeping the real-zerialize tests in their own target means
# only *that* target's (and BEVE's own, below) required standard moves;
# multi_index_lru_test never links `zerialize` at all, so it stays C++20
# unconditionally, matching every base/compatibility-matrix CI job.
if(MULTI_INDEX_LRU_BUILD_ZERIALIZE_TESTS)
    add_executable(zerialize_real_test zerialize_real_test.cpp)
    target_link_libraries(zerialize_real_test PRIVATE
        multi_index_lru::multi_index_lru
        zerialize
        GTest::gtest
        GTest::gtest_main
    )
    gtest_discover_tests(zerialize_real_test)
endif()

# A further separate target, not sources on zerialize_real_test above:
# BEVE's own beve.hpp is only needed here, and target_compile_features
# below is the only way to actually require C++23 for the code that uses
# it (zerialize_real_test picks up C++23 too when this option is on, but
# only as an unavoidable side effect of linking the same now-BEVE-enabled
# `zerialize` target -- see its own header comment -- not because anything
# in that file needs it).
if(MULTI_INDEX_LRU_BUILD_ZERIALIZE_BEVE_TESTS)
    add_executable(zerialize_beve_test zerialize_beve_test.cpp)
    target_compile_features(zerialize_beve_test PRIVATE cxx_std_23)
    target_link_libraries(zerialize_beve_test PRIVATE
        multi_index_lru::multi_index_lru
        zerialize
        GTest::gtest
        GTest::gtest_main
    )
    gtest_discover_tests(zerialize_beve_test)
endif()
