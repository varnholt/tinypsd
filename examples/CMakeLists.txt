add_executable(psd_layers psd_layers.cpp)
target_link_libraries(psd_layers PRIVATE tinypsd::tinypsd tinypsd_warnings)
