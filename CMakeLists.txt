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
set(MULTI_INDEX_LRU_BOOST_GIT_TAG "develop" CACHE STRING
    "Git tag or branch used for fetched Boost modules (for example develop or boost-1.91.0)")

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
