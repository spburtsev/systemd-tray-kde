cmake_minimum_required(VERSION 3.20)

project(systemd-tray LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(systemd-tray main.cpp)
target_link_libraries(systemd-tray PRIVATE Qt6::Widgets)

