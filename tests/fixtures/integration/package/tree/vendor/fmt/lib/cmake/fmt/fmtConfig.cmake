# Offline fixture stub for find_package(fmt 11 REQUIRED).
if(NOT TARGET fmt::fmt)
  add_library(fmt::fmt INTERFACE IMPORTED)
endif()
