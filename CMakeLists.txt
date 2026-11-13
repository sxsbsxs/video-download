cmake_minimum_required(VERSION 3.20)
project(videodl_next VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(HTTPLIB REQUIRED IMPORTED_TARGET cpp-httplib)
find_package(nlohmann_json 3.2.0 REQUIRED)
find_package(Threads REQUIRED)

add_executable(videodl src/main.cpp)
target_link_libraries(videodl PRIVATE
    PkgConfig::HTTPLIB
    nlohmann_json::nlohmann_json
    Threads::Threads
)
target_compile_options(videodl PRIVATE -Wall -Wextra -Wpedantic)
