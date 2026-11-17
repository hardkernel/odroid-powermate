# SPDX-FileCopyrightText: 2025 Hardkernel Co., Ltd.
# SPDX-License-Identifier: GPL-3.0-only

# Inputs (checked in)
set(PROTO_FILE ${CMAKE_SOURCE_DIR}/proto/status.proto)
set(WEB_APP_SOURCE_DIR ${CMAKE_SOURCE_DIR}/page)

# Outputs (generated into the build directory, never checked in)
set(PROTO_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto)
set(PROTO_C_FILE ${PROTO_OUT_DIR}/status.pb.c)
set(PROTO_H_FILE ${PROTO_OUT_DIR}/status.pb.h)
set(WEB_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/web)
set(WEB_PROTO_JS ${WEB_OUT_DIR}/proto.js)
set(GZ_OUTPUT_FILE ${WEB_OUT_DIR}/dist/index.html.gz)

# Build tools
find_program(NPM_EXECUTABLE npm)
if (NOT NPM_EXECUTABLE)
    message(FATAL_ERROR "npm not found! Please install Node.js and npm.")
endif ()
find_program(PROTOC_EXECUTABLE protoc)
find_program(NANOPB_PLUGIN protoc-gen-nanopb)
if (NOT PROTOC_EXECUTABLE OR NOT NANOPB_PLUGIN)
    message(FATAL_ERROR "protoc or protoc-gen-nanopb not found! Please install nanopb (Ubuntu: apt install nanopb).")
endif ()

idf_component_register(SRC_DIRS "app" "drivers" "nconfig" "wifi" "indicator" "service"
        INCLUDE_DIRS "include"
        EMBED_FILES ${GZ_OUTPUT_FILE}
)

target_sources(${COMPONENT_LIB} PRIVATE ${PROTO_C_FILE})
target_include_directories(${COMPONENT_LIB} PRIVATE ${PROTO_OUT_DIR})

# --- Protobuf C sources (nanopb) ---------------------------------------------
add_custom_command(
        OUTPUT ${PROTO_C_FILE} ${PROTO_H_FILE}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PROTO_OUT_DIR}
        COMMAND ${PROTOC_EXECUTABLE} --plugin=protoc-gen-nanopb=${NANOPB_PLUGIN}
                --nanopb_out=${PROTO_OUT_DIR} -I${CMAKE_SOURCE_DIR}/proto ${PROTO_FILE}
        DEPENDS ${PROTO_FILE}
        COMMENT "Generating nanopb sources from status.proto"
        VERBATIM
)
add_custom_target(protobuf_generate ALL DEPENDS ${PROTO_C_FILE} ${PROTO_H_FILE})

# --- Web UI -------------------------------------------------------------------
# Everything vite reads, globbed so that a new file under src/ is picked up too.
# src/proto.js is left out: it only exists when the page is built with npm alone.
file(GLOB_RECURSE WEB_APP_SOURCES CONFIGURE_DEPENDS ${WEB_APP_SOURCE_DIR}/src/*)
list(REMOVE_ITEM WEB_APP_SOURCES ${WEB_APP_SOURCE_DIR}/src/proto.js)
file(GLOB WEB_APP_PLUGINS CONFIGURE_DEPENDS ${WEB_APP_SOURCE_DIR}/plugins/*.js)
set(NPM_INSTALL_STAMP ${WEB_APP_SOURCE_DIR}/node_modules/.package-lock.json)
set(NPM_BIN ${WEB_APP_SOURCE_DIR}/node_modules/.bin)

# npm ci installs exactly package-lock.json and never rewrites it. It runs again only
# when the dependencies change or node_modules is gone.
add_custom_command(
        OUTPUT ${NPM_INSTALL_STAMP}
        COMMAND ${NPM_EXECUTABLE} ci
        COMMAND ${CMAKE_COMMAND} -E touch ${NPM_INSTALL_STAMP}
        WORKING_DIRECTORY ${WEB_APP_SOURCE_DIR}
        DEPENDS ${WEB_APP_SOURCE_DIR}/package.json ${WEB_APP_SOURCE_DIR}/package-lock.json
        COMMENT "Installing web UI dependencies (npm ci)"
        VERBATIM
)

# The page only decodes StatusMessage, so leave out encode, verify, toObject and the
# other generated helpers (keep these flags in step with build:proto in package.json).
add_custom_command(
        OUTPUT ${WEB_PROTO_JS}
        COMMAND ${NPM_BIN}/pbjs -t static-module -w es6
                --no-create --no-encode --no-verify --no-convert --no-delimited --no-typeurl --no-comments
                -o ${WEB_PROTO_JS} ${PROTO_FILE}
        WORKING_DIRECTORY ${WEB_APP_SOURCE_DIR}
        DEPENDS ${NPM_INSTALL_STAMP} ${PROTO_FILE}
        COMMENT "Generating proto.js from status.proto"
        VERBATIM
)

# vite.config.js takes the generated proto.js from POWERMATE_PROTO_JS.
add_custom_command(
        OUTPUT ${GZ_OUTPUT_FILE}
        COMMAND ${CMAKE_COMMAND} -E env POWERMATE_PROTO_JS=${WEB_PROTO_JS}
                ${NPM_BIN}/vite build --outDir ${WEB_OUT_DIR}/dist --emptyOutDir --logLevel warn
        WORKING_DIRECTORY ${WEB_APP_SOURCE_DIR}
        DEPENDS
        ${NPM_INSTALL_STAMP}
        ${WEB_PROTO_JS}
        ${WEB_APP_SOURCE_DIR}/vite.config.js
        ${WEB_APP_PLUGINS}
        ${WEB_APP_SOURCE_DIR}/index.html
        ${WEB_APP_SOURCES}
        COMMENT "Building web UI (vite build)"
        VERBATIM
)
add_custom_target(build_web_app ALL DEPENDS ${GZ_OUTPUT_FILE})

add_dependencies(${COMPONENT_LIB} build_web_app)
add_dependencies(${COMPONENT_LIB} protobuf_generate)
