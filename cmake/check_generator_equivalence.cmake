foreach(required CMAKEGEN_SOURCE_DIR CMAKEGEN_BOOTSTRAP_BINARY CMAKEGEN_NORMAL_BINARY)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

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
  if(NOT status EQUAL 0 OR NOT output STREQUAL "No changes.\n")
    message(FATAL_ERROR "${generator} does not reproduce canonical output (status ${status}): ${output}${error}")
  endif()
endforeach()
