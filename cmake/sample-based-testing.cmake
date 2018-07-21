message(STATUS "Looking for sample set for sample-based testing")
message(STATUS "Looking for sample set in ${RAWSPEED_REFERENCE_SAMPLE_ARCHIVE}")
if(NOT (EXISTS "${RAWSPEED_REFERENCE_SAMPLE_ARCHIVE}"
    AND EXISTS "${RAWSPEED_REFERENCE_SAMPLE_ARCHIVE}/filelist.sha1"
    AND EXISTS "${RAWSPEED_REFERENCE_SAMPLE_ARCHIVE}/timestamp.txt"))
  message(SEND_ERROR "Did not find sample set for sample-based testing! Either pass correct path in RAWSPEED_REFERENCE_SAMPLE_ARCHIVE, or disable RAWSPEED_ENABLE_SAMPLE_BASED_TESTING.")
endif()

message(STATUS "Found sample set in ${RAWSPEED_REFERENCE_SAMPLE_ARCHIVE}")

file(STRINGS "${RAWSPEED_REFERENCE_SAMPLE_ARCHIVE}/filelist.sha1" _REFERENCE_SAMPLES ENCODING UTF-8)

set(REFERENCE_SAMPLES)
set(REFERENCE_SAMPLE_HASHES)

foreach(STR ${_REFERENCE_SAMPLES})
  # There are two schemes:
  #   <hash><space><space><filename>      <- read in text mode
  #   <hash><space><asterisk><filename>   <- read in binary mode
  # Which for our purpose means:
  #   <40 chars><char><char><filename>    <- i.e. we just skip first 42 chars.
  string(SUBSTRING "${STR}" 42 -1 SAMPLENAME)
  set(SAMPLENAME "${RAWSPEED_REFERENCE_SAMPLE_ARCHIVE}/${SAMPLENAME}")

  if(NOT EXISTS "${SAMPLENAME}")
    message(SEND_ERROR "The sample \"${SAMPLENAME}\" does not exist!")
  endif()

  list(APPEND REFERENCE_SAMPLES "${SAMPLENAME}")
  list(APPEND REFERENCE_SAMPLE_HASHES "${SAMPLENAME}.hash")
  list(APPEND REFERENCE_SAMPLE_HASHES "${SAMPLENAME}.hash.failed")
endforeach()

add_custom_target(rstest-create)
add_custom_command(TARGET rstest-create
  COMMAND rstest -c ${REFERENCE_SAMPLES}
  WORKING_DIRECTORY "${PROJECT_BINARY_DIR}"
  COMMENT "Running rstest on all the samples in the sample set to generate the missing hashes"
  VERBATIM
  USES_TERMINAL)

add_custom_target(rstest-recreate)
add_custom_command(TARGET rstest-recreate
  COMMAND rstest -c -f ${REFERENCE_SAMPLES}
  WORKING_DIRECTORY "${PROJECT_BINARY_DIR}"
  COMMENT "Running rstest on all the samples in the sample set to [re]generate all the hashes"
  VERBATIM
  USES_TERMINAL)

add_custom_target(rstest-test) # hashes must exist beforehand
add_custom_command(TARGET rstest-test
  COMMAND rstest ${REFERENCE_SAMPLES}
  WORKING_DIRECTORY "${PROJECT_BINARY_DIR}"
  COMMENT "Running rstest on all the samples in the sample set to check for regressions"
  VERBATIM
  USES_TERMINAL)

add_custom_target(rstest-check) # hashes should exist beforehand if you want to check for regressions
add_custom_command(TARGET rstest-check
  COMMAND rstest -f ${REFERENCE_SAMPLES}
  WORKING_DIRECTORY "${PROJECT_BINARY_DIR}"
  COMMENT "Trying to decode all the samples in the sample set"
  VERBATIM
  USES_TERMINAL)

add_custom_target(rstest-clean)
add_custom_command(TARGET rstest-clean
  COMMAND "${CMAKE_COMMAND}" -E remove ${REFERENCE_SAMPLE_HASHES}
  WORKING_DIRECTORY "${PROJECT_BINARY_DIR}"
  COMMENT "Removing *.hash, *.hash.failed for the each sample in the set"
  VERBATIM)
