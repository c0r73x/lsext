if (LIBGIT2_LIBRARIES AND LIBGIT2_INCLUDE_DIRS)
    # in cache already
    set(LIBGIT2_FOUND TRUE)
else (LIBGIT2_LIBRARIES AND LIBGIT2_INCLUDE_DIRS)
    find_path(LIBGIT2_INCLUDE_DIR
        NAMES
            git2.h
        PATHS
            /usr/include
            /usr/local/include
            /opt/local/include
            /opt/homebrew/include
            /sw/include
    )

    find_library(LIBGIT2_LIBRARY
        NAMES
            git2
        PATHS
            /usr/lib
            /usr/local/lib
            /opt/local/lib
            /opt/homebrew/lib
            /sw/lib
    )

    if (LIBGIT2_LIBRARY)
        set(LIBGIT2_FOUND TRUE)
    endif (LIBGIT2_LIBRARY)

    set(LIBGIT2_INCLUDE_DIRS
        ${LIBGIT2_INCLUDE_DIR}
    )

    if (LIBGIT2_FOUND)
        set(LIBGIT2_LIBRARIES
            ${LIBGIT2_LIBRARIES}
            ${LIBGIT2_LIBRARY}
        )
    endif (LIBGIT2_FOUND)

    if (LIBGIT2_INCLUDE_DIRS AND LIBGIT2_LIBRARIES)
        set(LIBGIT2_FOUND TRUE)
    endif (LIBGIT2_INCLUDE_DIRS AND LIBGIT2_LIBRARIES)

    if (LIBGIT2_FOUND)
        if (NOT git2_FIND_QUIETLY)
            message(STATUS "Found libgit2: ${LIBGIT2_LIBRARIES}")
        endif (NOT git2_FIND_QUIETLY)
    else (LIBGIT2_FOUND)
        if (git2_FIND_REQUIRED)
            message(FATAL_ERROR "Could not find libgit2")
        endif (git2_FIND_REQUIRED)
    endif (LIBGIT2_FOUND)

    mark_as_advanced(LIBGIT2_INCLUDE_DIRS LIBGIT2_LIBRARIES)
endif (LIBGIT2_LIBRARIES AND LIBGIT2_INCLUDE_DIRS)
