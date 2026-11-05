add_executable(tinypsd_test psd_writer.h test_framework.h tinypsd_test.cpp zlib_vectors.inc)
target_link_libraries(tinypsd_test PRIVATE tinypsd::tinypsd tinypsd_warnings)
add_test(NAME tinypsd_test COMMAND tinypsd_test)

add_executable(tinypsd_reference_test reference_test.cpp test_framework.h)
target_link_libraries(tinypsd_reference_test PRIVATE tinypsd::tinypsd tinypsd_warnings)
target_compile_definitions(tinypsd_reference_test
                           PRIVATE TINYPSD_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_test(NAME tinypsd_reference_test COMMAND tinypsd_reference_test)
