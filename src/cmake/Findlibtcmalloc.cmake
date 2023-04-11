# Find tcmalloc Library
#
#  TCMALLOC_LIBRARIES    - List of libraries when using libtcmalloc.
#  TCMALLOC_FOUND        - True if libtcmalloc is found.

# TCMALLOC_LIBRARY
find_library(TCMALLOC_LIBRARY NAMES tcmalloc)

# handle the QUIETLY and REQUIRED arguments and set TCMALLOC_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(libtcmalloc REQUIRED_VARS TCMALLOC_LIBRARY)


if (TCMALLOC_FOUND)
  set(TCMALLOC_LIBRARIES    ${TCMALLOC_LIBRARY})
endif()

mark_as_advanced(
  TCMALLOC_LIBRARY
)
