add_executable(basic_usage basic_usage.cpp)
target_link_libraries(basic_usage PRIVATE multi_index_lru::multi_index_lru)

add_executable(user_cache user_cache.cpp)
target_link_libraries(user_cache PRIVATE multi_index_lru::multi_index_lru)

add_executable(zerialize_cache zerialize_cache.cpp)
target_link_libraries(zerialize_cache PRIVATE multi_index_lru::multi_index_lru)

add_executable(expirable_cache expirable_cache.cpp)
target_link_libraries(expirable_cache PRIVATE multi_index_lru::multi_index_lru)

if(MULTI_INDEX_LRU_BUILD_SBEPP_EXAMPLE)
    if(NOT DEFINED MULTI_INDEX_LRU_SBEPP_GENERATED_DIR)
        message(FATAL_ERROR "Set MULTI_INDEX_LRU_SBEPP_GENERATED_DIR to the directory with sbepp-generated headers")
    endif()

    find_package(sbepp CONFIG REQUIRED)

    add_executable(sbepp_cache sbepp_cache.cpp)
    target_link_libraries(sbepp_cache PRIVATE
        multi_index_lru::multi_index_lru
        sbepp::sbepp
    )
    target_include_directories(sbepp_cache PRIVATE
        ${MULTI_INDEX_LRU_SBEPP_GENERATED_DIR}
    )
endif()
