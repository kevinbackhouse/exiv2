set(INIReader_LIBRARY_NAMES "INIReader" "libINIReader")

find_path(INIReader_INCLUDE_DIR
  NAMES "INIReader.h"
  DOC "INIReader include directory")

find_library(INIReader_LIBRARY
  NAMES ${INIReader_LIBRARY_NAMES}
  DOC "inih library")

mark_as_advanced(INIReader_INCLUDE_DIR)
mark_as_advanced(INIReader_LIBRARY)

find_package_handle_standard_args(INIReader REQUIRED_VARS INIReader_LIBRARY INIReader_INCLUDE_DIR)

if(INIReader_FOUND)
  set(INIReader_INCLUDE_DIRS "${INIReader_INCLUDE_DIR}")
  set(INIReader_LIBRARIES "${INIReader_LIBRARY}")
  if(NOT TARGET INIReader::INIReader)
    add_library(INIReader::INIReader INTERFACE IMPORTED)
  endif()
  set_property(TARGET INIReader::INIReader PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${INIReader_INCLUDE_DIRS}")
  set_property(TARGET INIReader::INIReader PROPERTY INTERFACE_LINK_LIBRARIES "${INIReader_LIBRARIES}")
endif()
