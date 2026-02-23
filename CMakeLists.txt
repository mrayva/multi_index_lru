cmake_minimum_required(VERSION 3.20)

project(multi_index_lru
    VERSION 1.0.0
    DESCRIPTION "Header-only LRU container based on Boost.MultiIndex"
    HOMEPAGE_URL "https://github.com/yourusername/multi_index_lru"
    LANGUAGES CXX
)

# C++20 required
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Options
option(MULTI_INDEX_LRU_BUILD_TESTS "Build tests" ON)
option(MULTI_INDEX_LRU_BUILD_EXAMPLES "Build examples" ON)

# Find Boost (multi_index is header-only)
find_package(Boost 1.74 REQUIRED)

# Header-only library target
add_library(multi_index_lru INTERFACE)
add_library(multi_index_lru::multi_index_lru ALIAS multi_index_lru)

target_include_directories(multi_index_lru INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(multi_index_lru INTERFACE
    Boost::boost
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

# Installation
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS multi_index_lru
    EXPORT multi_index_lru-targets
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
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
