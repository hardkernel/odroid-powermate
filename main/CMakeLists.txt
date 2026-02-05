# Define the web application source directory and the final output file
set(WEB_APP_SOURCE_DIR ${CMAKE_SOURCE_DIR}/page)
set(GZ_OUTPUT_FILE ${WEB_APP_SOURCE_DIR}/dist/index.html.gz)

set(PROTO_DIR ${CMAKE_SOURCE_DIR}/proto)
set(PROTO_OUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/proto)
set(PROTO_FILE ${PROTO_DIR}/status.proto)
set(PROTO_C_FILE ${PROTO_OUT_DIR}/status.pb.c)
set(PROTO_H_FILE ${PROTO_OUT_DIR}/status.pb.h)

# Check npm is available
find_program(NPM_EXECUTABLE npm)
if (NOT NPM_EXECUTABLE)
    message(FATAL_ERROR "npm not found! Please install Node.js and npm.")
endif ()

# Register the component. Now, CMake knows how GZ_OUTPUT_FILE is generated
# and can correctly handle the dependency for embedding.
idf_component_register(SRC_DIRS "app" "nconfig" "wifi" "indicator" "service" "proto"
        INCLUDE_DIRS "include" "proto"
        EMBED_FILES ${GZ_OUTPUT_FILE}
)

target_sources(${COMPONENT_LIB} PRIVATE ${PROTO_C_FILE})

# Define a custom command to build the web app.
# This command explicitly tells CMake that it produces the GZ_OUTPUT_FILE.
add_custom_command(
        OUTPUT ${GZ_OUTPUT_FILE}
        COMMAND npm install
        COMMAND npm run build
        WORKING_DIRECTORY ${WEB_APP_SOURCE_DIR}
        # Re-run the build if any of these files change
        DEPENDS
        ${WEB_APP_SOURCE_DIR}/package.json
        ${WEB_APP_SOURCE_DIR}/vite.config.js
        ${WEB_APP_SOURCE_DIR}/index.html
        ${WEB_APP_SOURCE_DIR}/src/api.js
        ${WEB_APP_SOURCE_DIR}/src/chart.js
        ${WEB_APP_SOURCE_DIR}/src/dom.js
        ${WEB_APP_SOURCE_DIR}/src/events.js
        ${WEB_APP_SOURCE_DIR}/src/main.js
        ${WEB_APP_SOURCE_DIR}/src/style.css
        ${WEB_APP_SOURCE_DIR}/src/terminal.js
        ${WEB_APP_SOURCE_DIR}/src/ui.js
        ${WEB_APP_SOURCE_DIR}/src/utils.js
        ${WEB_APP_SOURCE_DIR}/src/websocket.js

        COMMENT "Building Node.js project (npm install && npm run build)"
        VERBATIM
)

# Create a target that depends on the output file. When this target is built,
# it ensures the custom command above is executed first.
add_custom_target(build_web_app ALL
        DEPENDS ${GZ_OUTPUT_FILE}
)

add_custom_command(
        OUTPUT ${PROTO_C_FILE} ${PROTO_H_FILE}
        COMMAND protoc --nanopb_out=${PROTO_OUT_DIR} status.proto
        WORKING_DIRECTORY ${PROTO_DIR}
        DEPENDS ${PROTO_FILE}
        COMMENT "Generating C sources from ${PROTO_FILE} using nanopb"
        VERBATIM
)

add_custom_target(protobuf_generate ALL
        DEPENDS ${PROTO_C_FILE} ${PROTO_H_FILE}
)

add_dependencies(${COMPONENT_LIB} build_web_app)
add_dependencies(${COMPONENT_LIB} protobuf_generate)
