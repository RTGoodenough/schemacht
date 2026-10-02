# Runs one example against the test database: cmake -DEXAMPLE=<the example's executable> -P run_example.cmake
#
# The conninfo is $ENV{SCHEMACHT_TEST_DB}; without it the test is skipped, as the integration tests are. It fails if the
# example exits non-zero or writes anything to stderr (an error, or a notice that should have been dropped).

if(NOT DEFINED ENV{SCHEMACHT_TEST_DB} OR "$ENV{SCHEMACHT_TEST_DB}" STREQUAL "")
  message("SCHEMACHT_TEST_DB is not set: skipped")
  return()
endif()

execute_process(
  COMMAND "${EXAMPLE}" "$ENV{SCHEMACHT_TEST_DB}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE errors
)
message("${output}")
if(NOT result EQUAL 0)
  message(FATAL_ERROR "the example exited with ${result}:\n${errors}")
endif()
if(NOT errors STREQUAL "")
  message(FATAL_ERROR "the example wrote to stderr:\n${errors}")
endif()
