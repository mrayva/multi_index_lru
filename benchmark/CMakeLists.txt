include(FetchContent)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    google_benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.5
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(google_benchmark)

add_executable(multi_index_lru_benchmark container_benchmark.cpp)
target_link_libraries(multi_index_lru_benchmark PRIVATE
    multi_index_lru::multi_index_lru
    benchmark::benchmark_main
)

if(MSVC)
    target_compile_options(multi_index_lru_benchmark PRIVATE /W4 /WX)
else()
    target_compile_options(multi_index_lru_benchmark PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
