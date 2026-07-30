set(PMTUI_GO_EXECUTABLE "go" CACHE STRING "Go compiler used to build PMTUI")
set(PMTUI_BINARY "${CMAKE_BINARY_DIR}/pmtui")

add_custom_target(
        pmtui
        COMMAND "${PMTUI_GO_EXECUTABLE}" build -o "${PMTUI_BINARY}" .
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Building PMTUI to ${PMTUI_BINARY}"
        VERBATIM USES_TERMINAL
)
