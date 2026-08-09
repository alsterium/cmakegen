if(NOT DEFINED CMAKEGEN_SOURCE_DIR)
  message(FATAL_ERROR "CMAKEGEN_SOURCE_DIR is required")
endif()

file(GLOB_RECURSE cmakegen_implementation_sources
  RELATIVE "${CMAKEGEN_SOURCE_DIR}"
  "${CMAKEGEN_SOURCE_DIR}/src/*.cpp")
file(READ "${CMAKEGEN_SOURCE_DIR}/bootstrap/CMakeLists.txt" bootstrap_contents)

foreach(source IN LISTS cmakegen_implementation_sources)
  string(FIND "${bootstrap_contents}" "../${source}" source_position)
  if(source_position EQUAL -1)
    message(FATAL_ERROR "bootstrap/CMakeLists.txt is missing source: ${source}")
  endif()
endforeach()
