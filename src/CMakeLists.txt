cmake_minimum_required(VERSION 2.8)

project(lsext)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17 -Wall -ggdb")

set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)
set(CURSES_NEED_NCURSES TRUE)
set(CURSES_NEED_WIDE TRUE)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

find_package(git2 REQUIRED)
find_package(Iniparser REQUIRED)

set(COMMON_SRC
    "main.cpp"
    "entry.cpp"
)

include_directories(
    "src/"
    ${GIT2_INCLUDE_DIR}
    ${INIPARSER_INCLUDE_DIR}
)

add_executable(${CMAKE_PROJECT_NAME} ${COMMON_SRC})
install(TARGETS ${CMAKE_PROJECT_NAME} RUNTIME DESTINATION bin)

target_link_libraries(
    ${CMAKE_PROJECT_NAME}
    ${GIT2_LIBRARY}
    ${INIPARSER_LIBRARY}
)
