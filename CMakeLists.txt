cmake_minimum_required(VERSION 3.20)

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
option(MULTI_INDEX_LRU_USE_BOOST_DEVELOP "Use Boost.MultiIndex develop branch (pre-1.91 refactored)" OFF)

# Boost.MultiIndex handling
if(MULTI_INDEX_LRU_USE_BOOST_DEVELOP)
    message(STATUS "Using Boost.MultiIndex from develop branch (refactored for Boost 1.91+)")
    message(STATUS "This uses C++11 variadic templates and Boost.Mp11 instead of Boost.MPL")
    
    include(FetchContent)
    
    # List of required Boost modules for multi_index
    set(BOOST_MODULES
        multi_index config assert core type_traits static_assert
        throw_exception preprocessor move mp11 tuple container_hash
        describe bind iterator mpl utility smart_ptr integer detail
        io function_types fusion typeof functional concept_check optional
    )
    
    # Declare all modules
    foreach(module ${BOOST_MODULES})
        FetchContent_Declare(
            boost_${module}
            GIT_REPOSITORY https://github.com/boostorg/${module}.git
            GIT_TAG develop
            SOURCE_DIR ${CMAKE_BINARY_DIR}/_deps/boost_${module}
        )
    endforeach()
    
    # Populate all modules (download only, don't add_subdirectory)
    foreach(module ${BOOST_MODULES})
        FetchContent_GetProperties(boost_${module})
        if(NOT boost_${module}_POPULATED)
            FetchContent_Populate(boost_${module})
        endif()
    endforeach()

    # Create interface library for fetched Boost headers
    add_library(boost_multi_index_develop INTERFACE)
    foreach(module ${BOOST_MODULES})
        target_include_directories(boost_multi_index_develop INTERFACE
            ${CMAKE_BINARY_DIR}/_deps/boost_${module}/include
        )
    endforeach()
    
    set(BOOST_TARGET boost_multi_index_develop)
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
endif()
