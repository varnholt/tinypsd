# builds against an installed tinypsd to check that find_package works and pulls in nothing but the
# header. cmake --install <build> --prefix <prefix> cmake -S tests/package_consumer -B <out>
# -DCMAKE_PREFIX_PATH=<prefix>
cmake_minimum_required(VERSION 3.25)
project(tinypsd_package_consumer LANGUAGES CXX)

find_package(tinypsd 1 REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE tinypsd::tinypsd)

# the package exports exactly one interface target with only an include directory and the c++
# standard
get_target_property(link_libraries tinypsd::tinypsd INTERFACE_LINK_LIBRARIES)
if(link_libraries)
    message(FATAL_ERROR "tinypsd::tinypsd links ${link_libraries}, it must not depend on anything")
endif()
