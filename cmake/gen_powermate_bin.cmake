if (NOT TARGET powermate_gen_single_bin)
    add_custom_target(
            powermate_gen_single_bin
            COMMAND ${CMAKE_COMMAND} -E echo "Merge bin files to ${PROJECT_VER}.bin"
            COMMAND ${ESPTOOLPY} --chip ${IDF_TARGET} merge_bin -o ${PROJECT_VER}.bin @flash_args
            COMMAND ${CMAKE_COMMAND} -E echo "Merge bin done"
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            DEPENDS gen_project_binary bootloader
            VERBATIM USES_TERMINAL
    )
endif()

# Flash bin ${PROJECT_VER}.bin to target chip
if (NOT TARGET powermate_flash_single_bin)
    add_custom_target(
            powermate_flash_single_bin
            COMMAND ${CMAKE_COMMAND} -E echo "Flash merged bin ${PROJECT_VER}.bin to address 0x0"
            COMMAND ${ESPTOOLPY} --chip ${IDF_TARGET} write_flash 0x0 ${PROJECT_VER}.bin
            COMMAND ${CMAKE_COMMAND} -E echo "Flash merged bin done"
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            DEPENDS powermate_gen_single_bin
            VERBATIM USES_TERMINAL
    )
endif()