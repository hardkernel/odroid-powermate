idf_component_get_property(esptool_py_cmd esptool_py ESPTOOLPY_CMD)
if (NOT esptool_py_cmd)
    message(FATAL_ERROR "ESP-IDF esptool command is unavailable")
endif()

set(powermate_single_bin_name "powermate-${PROJECT_VER}.bin")
set(powermate_single_bin "${CMAKE_BINARY_DIR}/${powermate_single_bin_name}")
set(powermate_flash_args "${CMAKE_BINARY_DIR}/flash_args")

if (NOT TARGET powermate_gen_single_bin)
    add_custom_target(
            powermate_gen_single_bin
            COMMAND ${CMAKE_COMMAND} -E echo "Merge bin files to ${powermate_single_bin_name}"
            COMMAND ${esptool_py_cmd} merge-bin -o "${powermate_single_bin}" "@${powermate_flash_args}"
            COMMAND ${CMAKE_COMMAND} -E echo "Merge bin done"
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            DEPENDS gen_project_binary bootloader
            BYPRODUCTS "${powermate_single_bin}"
            VERBATIM USES_TERMINAL
    )
endif()

# Flash the merged binary to the target chip.
if (NOT TARGET powermate_flash_single_bin)
    add_custom_target(
            powermate_flash_single_bin
            COMMAND ${CMAKE_COMMAND} -E echo "Flash merged bin ${powermate_single_bin_name} to address 0x0"
            COMMAND ${esptool_py_cmd} write-flash 0x0 "${powermate_single_bin}"
            COMMAND ${CMAKE_COMMAND} -E echo "Flash merged bin done"
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            DEPENDS powermate_gen_single_bin
            VERBATIM USES_TERMINAL
    )
endif()
