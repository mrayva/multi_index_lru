cmake_minimum_required(VERSION 3.20)
project(multi_index_lru_package_consumer LANGUAGES CXX)

find_package(multi_index_lru 1.0 REQUIRED CONFIG)

add_executable(package_consumer main.cpp)
target_link_libraries(package_consumer PRIVATE multi_index_lru::multi_index_lru)
