include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(multi_index_lru_test
    container_test.cpp
    zerialize_test.cpp
    expirable_test.cpp
)

target_link_libraries(multi_index_lru_test PRIVATE
    multi_index_lru::multi_index_lru
    GTest::gtest
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(multi_index_lru_test)
