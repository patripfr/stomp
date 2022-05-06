cmake_minimum_required(VERSION 2.8.3)
project(stomp)

find_package(catkin_simple REQUIRED)
catkin_simple()

add_definitions(-std=c++14)
cs_add_library(${PROJECT_NAME}_lib
    src/utils.cpp
    src/stomp.cpp
)

cs_add_executable(${PROJECT_NAME}_example examples/stomp_example.cpp)
target_link_libraries(${PROJECT_NAME}_example ${PROJECT_NAME}_lib)

cs_install()
cs_export()
