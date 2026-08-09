# Hand-maintained normal-build extensions. project.yml owns targets and their
# sources; this file owns the CTest/Catch2 integration not represented by the
# schema-v1 model.
include(CTest)

if(BUILD_TESTING)
  include(Catch)

  target_compile_definitions(cmakegen_tests PRIVATE
    CMAKEGEN_CMAKE_COMMAND="${CMAKE_COMMAND}"
    CMAKEGEN_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  )
  catch_discover_tests(cmakegen_tests)

  add_test(
    NAME cmakegen-bootstrap-source-drift
    COMMAND "${CMAKE_COMMAND}"
      "-DCMAKEGEN_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
      -P "${CMAKE_SOURCE_DIR}/cmake/check_bootstrap_sources.cmake"
  )

  # CI supplies the stage-0 executable after building bootstrap. Each binary
  # must accept the checked-in generated output, proving canonical equivalence.
  if(CMAKEGEN_BOOTSTRAP_BINARY)
    add_test(
      NAME cmakegen-bootstrap-normal-output-equivalence
      COMMAND "${CMAKE_COMMAND}"
        "-DCMAKEGEN_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        "-DCMAKEGEN_BOOTSTRAP_BINARY=${CMAKEGEN_BOOTSTRAP_BINARY}"
        "-DCMAKEGEN_NORMAL_BINARY=$<TARGET_FILE:cmakegen>"
        -P "${CMAKE_SOURCE_DIR}/cmake/check_generator_equivalence.cmake"
    )
  endif()
endif()
