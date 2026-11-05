if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "TINYPSD_BUILD_FUZZER needs clang (libFuzzer)")
endif()

add_executable(tinypsd_fuzz fuzz_load.cpp)
target_link_libraries(tinypsd_fuzz PRIVATE tinypsd::tinypsd tinypsd_warnings)
target_compile_options(tinypsd_fuzz PRIVATE -g -fsanitize=fuzzer,address,undefined
                                            -fno-sanitize-recover=all)
target_link_options(tinypsd_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
