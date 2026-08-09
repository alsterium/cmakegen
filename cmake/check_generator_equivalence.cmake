foreach(required CMAKEGEN_SOURCE_DIR CMAKEGEN_BOOTSTRAP_BINARY CMAKEGEN_NORMAL_BINARY)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

# CTest starts this script from the normal build directory. Resolve a stage-0
# path supplied relative to the source tree before checking or invoking it.
if(NOT IS_ABSOLUTE "${CMAKEGEN_BOOTSTRAP_BINARY}")
  get_filename_component(
    CMAKEGEN_BOOTSTRAP_BINARY
    "${CMAKEGEN_SOURCE_DIR}/${CMAKEGEN_BOOTSTRAP_BINARY}"
    ABSOLUTE)
endif()

foreach(generator CMAKEGEN_BOOTSTRAP_BINARY CMAKEGEN_NORMAL_BINARY)
  if(NOT EXISTS "${${generator}}")
    message(FATAL_ERROR "${generator} does not exist: ${${generator}}")
  endif()
  execute_process(
    COMMAND "${${generator}}" --project project.yml --root "${CMAKEGEN_SOURCE_DIR}" generate --check --no-color
    WORKING_DIRECTORY "${CMAKEGEN_SOURCE_DIR}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
  # Exit status is the canonical check result.  Do not compare the friendly
  # diagnostic byte-for-byte: redirected stdout uses CRLF on Windows.
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "${generator} does not reproduce canonical output (status ${status}): ${output}${error}")
  endif()
endforeach()
