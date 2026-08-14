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

if(MULTI_INDEX_LRU_BUILD_ZERIALIZE_TESTS)
    target_sources(multi_index_lru_test PRIVATE zerialize_real_test.cpp)
    target_link_libraries(multi_index_lru_test PRIVATE zerialize)
endif()

include(GoogleTest)
gtest_discover_tests(multi_index_lru_test)
