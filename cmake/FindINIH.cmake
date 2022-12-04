set(INIH_LIBRARY_NAMES "inih" "libinih")

find_path(INIH_INCLUDE_DIR
  NAMES "ini.h"
  DOC "inih include directory")

find_library(INIH_LIBRARY
  NAMES ${INIH_LIBRARY_NAMES}
  DOC "inih library")

mark_as_advanced(INIH_INCLUDE_DIR)
mark_as_advanced(INIH_LIBRARY)

find_package_handle_standard_args(INIH REQUIRED_VARS INIH_LIBRARY INIH_INCLUDE_DIR)

if(INIH_FOUND)
  set(INIH_INCLUDE_DIRS "${INIH_INCLUDE_DIR}")
  set(INIH_LIBRARIES "${INIH_LIBRARY}")
  if(NOT TARGET INIH::INIH)
    add_library(INIH::INIH INTERFACE IMPORTED)
  endif()
  set_property(TARGET INIH::INIH PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${INIH_INCLUDE_DIRS}")
  set_property(TARGET INIH::INIH PROPERTY INTERFACE_LINK_LIBRARIES "${INIH_LIBRARIES}")
endif()
