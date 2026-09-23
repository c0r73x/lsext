cmake_minimum_required(VERSION 3.10)

project(lsext)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_CXX_STANDARD 17)
set(CXX_STANDARD_REQUIRED)

option(USE_OPENMP "Enable OpenMP threading" ON)
option(USE_GIT "Enable GIT" ON)
option(USE_TCMALLOC "Use libtcmalloc" ON)
option(USE_DYNAMIC_LIBGIT2 "Use system libgit2" OFF)
option(USE_LTO "Enable LTO" ON)

if(USE_LTO)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()

add_definitions("-Werror -Wall -pedantic -Wno-unknown-pragmas -Wno-error=strict-aliasing -Wno-error=unused-variable -Wno-error=unused-function")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

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

        if (LIBGIT2_FOUND)
            set (GIT_INCLUDE_DIR "${LIBGIT2_INCLUDE_DIR}")
            set (GIT_LIBRARY "${LIBGIT2_LIBRARIES}")
        endif()
    else()
        set(CMAKE_EXE_LINKER_FLAGS
            "${CMAKE_EXE_LINKER_FLAGS} -Wl,-rpath -Wl,/usr/local/lib"
        )

        set (GIT_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../externals/libgit2/include")
        set (GIT_LIBRARY "${CMAKE_CURRENT_LIST_DIR}/../externals/libgit2/build/libgit2.a")


        find_package(Threads REQUIRED)
        list(APPEND LIBGIT2_LIBS "z" Threads::Threads)

        # the bundled fork has extra speed options (index checksum, ...)
        add_definitions(-DLSEXT_GIT_FORK)
    endif()

    add_definitions(-DUSE_GIT)
endif()

# add_compile_options(-fsanitize=thread)
# add_link_options(-fsanitize=thread)

find_package(Iniparser REQUIRED)

set(COMMON_SRC
    "main.cpp"
    "entry.cpp"
)

include_directories(
    "../src/"
    "../externals/gsl-lite/"
    "../externals/stb/"
    ${GIT_INCLUDE_DIR}
    ${INIPARSER_INCLUDE_DIRS}
)

add_executable(${CMAKE_PROJECT_NAME} ${COMMON_SRC})
install(TARGETS ${CMAKE_PROJECT_NAME} RUNTIME DESTINATION bin)

target_link_libraries(
    ${CMAKE_PROJECT_NAME}
    ${GIT_LIBRARY}
    ${LIBGIT2_LIBS}
    ${INIPARSER_LIBRARIES}
    ${OpenMP_CXX_LIBRARIES}
    ${TCMALLOC_LIBRARY}
)
