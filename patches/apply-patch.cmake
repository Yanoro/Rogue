# Idempotently apply a unified diff inside a git checkout.
#
# Used as the PATCH_COMMAND for FetchContent dependencies so that re-running the
# patch step (for example after a stamp is lost, or when the patch was already
# applied by hand) never fails and never double-applies.
#
# Usage:
#   cmake -DSRC_DIR=<checkout> -DPATCH_FILE=<patch> -P apply-patch.cmake
#
# "Already applied" is detected with a reverse check: if the patch can be
# cleanly reversed, its changes are present and there is nothing to do.

if(NOT SRC_DIR OR NOT PATCH_FILE)
    message(FATAL_ERROR "apply-patch.cmake requires -DSRC_DIR=<dir> -DPATCH_FILE=<patch>")
endif()

if(NOT EXISTS "${PATCH_FILE}")
    message(FATAL_ERROR "Patch file does not exist: ${PATCH_FILE}")
endif()

if(NOT EXISTS "${SRC_DIR}")
    message(FATAL_ERROR "Source directory does not exist: ${SRC_DIR}")
endif()

execute_process(
    COMMAND git apply --reverse --check "${PATCH_FILE}"
    WORKING_DIRECTORY "${SRC_DIR}"
    RESULT_VARIABLE _already_applied
    OUTPUT_QUIET
    ERROR_QUIET
)

if(_already_applied EQUAL 0)
    message(STATUS "Patch already applied, skipping: ${PATCH_FILE}")
    return()
endif()

execute_process(
    COMMAND git apply "${PATCH_FILE}"
    WORKING_DIRECTORY "${SRC_DIR}"
    RESULT_VARIABLE _apply_result
)

if(NOT _apply_result EQUAL 0)
    message(FATAL_ERROR "Failed to apply '${PATCH_FILE}' in '${SRC_DIR}'")
endif()

message(STATUS "Applied patch: ${PATCH_FILE}")
