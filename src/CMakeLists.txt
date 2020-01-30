cmake_minimum_required(VERSION 3.10)

project(lsext)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_CXX_STANDARD 14)
set(CXX_STANDARD_REQUIRED)

add_definitions("-Werror -Wall -pedantic -Wno-unknown-pragmas")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

option(USE_OPENMP "Enable OpenMP threading" ON)
option(USE_GIT "Enable GIT" ON)

if(USE_OPENMP)
    find_package(OpenMP REQUIRED)

    if (OPENMP_FOUND)
        add_definitions(-DUSE_OPENMP)

        set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS}")
        set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")
    endif()
endif()

if(USE_GIT)
    find_package(git2 REQUIRED)

    add_definitions(-DUSE_GIT)

    set (GIT_INCLUDE_DIR "${GIT2_INCLUDE_DIR}")
    set (GIT_LIBRARY "${GIT2_LIBRARY}")
endif()

find_package(Iniparser REQUIRED)
find_package(RE2 REQUIRED)

set(COMMON_SRC
    "main.cpp"
    "entry.cpp"
)

include_directories(
    "../src/"
    "../externals/gsl-lite/"
    ${GIT_INCLUDE_DIR}
    ${RE2_INCLUDE_DIR}
    ${INIPARSER_INCLUDE_DIRS}
)

add_executable(${CMAKE_PROJECT_NAME} ${COMMON_SRC})
install(TARGETS ${CMAKE_PROJECT_NAME} RUNTIME DESTINATION bin)

target_link_libraries(
    ${CMAKE_PROJECT_NAME}
    ${GIT_LIBRARY}
    ${RE2_LIBRARY}
    ${INIPARSER_LIBRARIES}
    ${OpenMP_CXX_LIBRARIES}
)
