cmake_minimum_required(VERSION 3.10)

project(lsext)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_CXX_STANDARD 11)
set(CXX_STANDARD_REQUIRED)

add_definitions("-Werror -Wall -pedantic -Wno-unknown-pragmas -Wno-error=strict-aliasing -Wno-error=unused-variable -Wno-error=unused-function")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

option(USE_OPENMP "Enable OpenMP threading" ON)
option(USE_GIT "Enable GIT" ON)
option(USE_TCMALLOC "Use libtcmalloc" ON)
option(USE_DYNAMIC_LIBGIT2 "Use system libgit2" OFF)

if(USE_OPENMP)
    find_package(OpenMP REQUIRED)

    if (OPENMP_FOUND)
        add_definitions(-DUSE_OPENMP)

        set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS}")
        set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")
    endif()
else()
    set (OpenMP_CXX_LIBRARIES "")
endif()

if(USE_TCMALLOC)
    find_package(libtcmalloc REQUIRED)

    if (TCMALLOC_FOUND)
        add_definitions(-DUSE_TCMALLOC)

        set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${TCMALLOC_C_FLAGS}")
        set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${TCMALLOC_CXX_FLAGS}")
    endif()
else()
    set (TCMALLOC_LIBRARY "")
endif()

if(USE_GIT)
    if(USE_DYNAMIC_LIBGIT2)
        find_package(git2 REQUIRED)

        set (GIT_INCLUDE_DIR "${GIT2_INCLUDE_DIR}")
        set (GIT_LIBRARY "${GIT2_LIBRARY}")
    else()
        set(CMAKE_EXE_LINKER_FLAGS
            "${CMAKE_EXE_LINKER_FLAGS} -Wl,-rpath -Wl,/usr/local/lib"
        )
        add_definitions(-DUSE_GIT)

        set (GIT_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../externals/libgit2/includes")
        set (GIT_LIBRARY "${CMAKE_CURRENT_LIST_DIR}/../externals/libgit2/build/libgit2.a")


        list(APPEND LIBGIT2_LIBS "z")
    endif()
endif()

if(CMAKE_C_FLAGS_RELEASE MATCHES "-O3")
  message(STATUS "Replacing -O3 in CMAKE_C_FLAGS_RELEASE with -Ofast")
  string(REPLACE "-O3" "-Ofast" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
endif()

find_package(Iniparser REQUIRED)
find_package(RE2 REQUIRED)

message (${RE2_LIBRARY})

set(COMMON_SRC
    "main.cpp"
    "entry.cpp"
)

include_directories(
    "../src/"
    "../externals/gsl-lite/"
    "../externals/stb/"
    ${GIT_INCLUDE_DIR}
    ${LIBGIT2_SYSTEM_INCLUDES}
    ${RE2_INCLUDE_DIR}
    ${INIPARSER_INCLUDE_DIRS}
)

add_executable(${CMAKE_PROJECT_NAME} ${COMMON_SRC})
install(TARGETS ${CMAKE_PROJECT_NAME} RUNTIME DESTINATION bin)

target_link_libraries(
    ${CMAKE_PROJECT_NAME}
    ${GIT_LIBRARY}
    ${LIBGIT2_LIBS}
    ${RE2_LIBRARY}
    ${INIPARSER_LIBRARIES}
    ${OpenMP_CXX_LIBRARIES}
    ${TCMALLOC_LIBRARY}
)
