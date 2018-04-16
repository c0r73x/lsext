cmake_minimum_required(VERSION 3.10)

project(lsext)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_CXX_STANDARD 17)
add_definitions("-Werror -Wall -pedantic")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

option(USEOPENMP "Enable OpenMP threading" ON)
if(USEOPENMP)
    find_package(OpenMP)
    if (OPENMP_FOUND)
        set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS}")
        set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")
    endif()
endif()

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
    pcrecpp
)
