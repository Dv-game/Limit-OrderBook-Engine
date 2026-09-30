cmake_minimum_required(VERSION 3.20)

project(Advanced_engine)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(Advanced_engine main.cpp)

target_include_directories(Advanced_engine PRIVATE
        ${CMAKE_SOURCE_DIR}
)