find_path(Xxhash_INCLUDE_DIR xxhash.h)
find_library(Xxhash_LIBRARY NAMES xxhash)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Xxhash DEFAULT_MSG
    Xxhash_INCLUDE_DIR Xxhash_LIBRARY)

if(Xxhash_FOUND)
    set(Xxhash_INCLUDE_DIRS ${Xxhash_INCLUDE_DIR})
    set(Xxhash_LIBRARIES ${Xxhash_LIBRARY})
endif()