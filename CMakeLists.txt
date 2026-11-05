cmake_minimum_required(VERSION 3.25)

project(
    tinypsd
    VERSION 1.0.0
    DESCRIPTION "Adobe Photoshop (.psd) loader, C++23, single header"
    HOMEPAGE_URL "https://github.com/varnholt/tinypsd"
    LANGUAGES CXX)

option(TINYPSD_BUILD_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})
option(TINYPSD_BUILD_EXAMPLES "Build the examples" ${PROJECT_IS_TOP_LEVEL})
option(TINYPSD_BUILD_FUZZER "Build the libFuzzer target (clang only)" OFF)
option(TINYPSD_INSTALL "Generate the install target" ${PROJECT_IS_TOP_LEVEL})

include(GNUInstallDirs)

add_library(tinypsd INTERFACE)
add_library(tinypsd::tinypsd ALIAS tinypsd)

target_include_directories(tinypsd INTERFACE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
                                             $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(tinypsd INTERFACE cxx_std_23)

# warnings for our own targets only, never forced onto consumers
if(TINYPSD_BUILD_TESTS
   OR TINYPSD_BUILD_EXAMPLES
   OR TINYPSD_BUILD_FUZZER)
    add_library(tinypsd_warnings INTERFACE)
    if(MSVC)
        target_compile_options(tinypsd_warnings INTERFACE /W4 /WX /permissive- /utf-8)
    else()
        target_compile_options(tinypsd_warnings INTERFACE -Wall -Wextra -Wpedantic -Wconversion
                                                          -Wshadow -Werror)
    endif()
endif()

if(TINYPSD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(TINYPSD_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(TINYPSD_BUILD_FUZZER)
    add_subdirectory(tests/fuzz)
endif()

if(TINYPSD_INSTALL)
    include(CMakePackageConfigHelpers)

    install(TARGETS tinypsd EXPORT tinypsdTargets)
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(
        EXPORT tinypsdTargets
        NAMESPACE tinypsd::
        FILE tinypsdConfig.cmake
        DESTINATION ${CMAKE_INSTALL_DATADIR}/cmake/tinypsd)

    write_basic_package_version_file(${PROJECT_BINARY_DIR}/tinypsdConfigVersion.cmake
                                     COMPATIBILITY SameMajorVersion ARCH_INDEPENDENT)
    install(FILES ${PROJECT_BINARY_DIR}/tinypsdConfigVersion.cmake
            DESTINATION ${CMAKE_INSTALL_DATADIR}/cmake/tinypsd)
endif()
