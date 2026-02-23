add_executable(basic_usage basic_usage.cpp)
target_link_libraries(basic_usage PRIVATE multi_index_lru::multi_index_lru)

add_executable(user_cache user_cache.cpp)
target_link_libraries(user_cache PRIVATE multi_index_lru::multi_index_lru)

add_executable(zerialize_cache zerialize_cache.cpp)
target_link_libraries(zerialize_cache PRIVATE multi_index_lru::multi_index_lru)

add_executable(expirable_cache expirable_cache.cpp)
target_link_libraries(expirable_cache PRIVATE multi_index_lru::multi_index_lru)
