# Register one complete installation and one application-only update. Never
# include the retained test image in an application-only update.
idf_build_get_property(recovery_build_dir BUILD_DIR)
idf_build_get_property(recovery_app_name EXECUTABLE_NAME)
set(recovery_app_bin "${recovery_app_name}.bin")
set(recovery_bundle "${CMAKE_SOURCE_DIR}/third-party/esp-mosaico-utils/esp-mosaico-recovery/firmware/recovery/prebuilt/recovery")
set(recovery_output "${recovery_build_dir}/recovery")
if(NOT EXISTS "${recovery_bundle}/manifest.json")
    message(FATAL_ERROR "Recovery bundle missing: run git submodule update --init third-party/esp-mosaico-utils")
endif()

# Blank otadata selects Claw instead of the upstream Vibe Mode marker.
string(ASCII 255 recovery_erased_byte)
string(REPEAT "${recovery_erased_byte}" 8192 recovery_otadata)
string(REPEAT "${recovery_erased_byte}" 24576 recovery_nvs)
string(REPEAT "${recovery_erased_byte}" 81920 recovery_sysmeta)
file(GENERATE OUTPUT "${recovery_output}/ota_data_claw.bin" CONTENT "${recovery_otadata}")
file(GENERATE OUTPUT "${recovery_output}/nvs_initial.bin" CONTENT "${recovery_nvs}")
file(GENERATE OUTPUT "${recovery_output}/sysmeta_initial.bin" CONTENT "${recovery_sysmeta}")

add_custom_target(retained-recovery ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${recovery_output}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${recovery_bundle}/bootloader.bin" "${recovery_bundle}/factory.bin"
        "${recovery_bundle}/manifest.json" "${recovery_output}"
    BYPRODUCTS "${recovery_output}/bootloader.bin" "${recovery_output}/factory.bin"
        "${recovery_output}/manifest.json"
    DEPENDS partition_table_bin
    COMMENT "Staging retained Recovery and initial Claw data"
    VERBATIM)

# Capture generated UI/SYSTEM paths before replacing IDF's installation list.
get_target_property(recovery_flash_entries flash FLASH_ENTRY)
foreach(entry IN LISTS recovery_flash_entries)
    if(entry MATCHES "^\"(ui_apps|system)\" :")
        set(label "${CMAKE_MATCH_1}")
        string(JSON relative_path GET "{${entry}}" "${label}" file)
        set(recovery_${label}_image "${recovery_build_dir}/${relative_path}")
    endif()
endforeach()
if(NOT recovery_ui_apps_image OR NOT recovery_system_image)
    message(FATAL_ERROR "Retained Recovery installation requires ui_apps and system images")
endif()

foreach(target flash app-flash bootloader-flash)
    foreach(property FLASH_FILE FLASH_ENTRY IMAGES)
        set_property(TARGET ${target} PROPERTY ${property} "")
    endforeach()
endforeach()

esptool_py_flash_to_partition(app-flash main_app "${recovery_build_dir}/${recovery_app_bin}")
esptool_py_flash_target_image(bootloader-flash bootloader 0x2000 "${recovery_output}/bootloader.bin")
add_dependencies(bootloader-flash retained-recovery)

# 'flash' is an INITIAL INSTALL: resets application NVS and Recovery sysmeta.
esptool_py_flash_target_image(flash bootloader 0x2000 "${recovery_output}/bootloader.bin")
esptool_py_flash_target_image(flash partition-table 0x8000 "${recovery_build_dir}/partition_table/partition-table.bin")
esptool_py_flash_to_partition(flash otadata "${recovery_output}/ota_data_claw.bin")
esptool_py_flash_to_partition(flash sysmeta "${recovery_output}/sysmeta_initial.bin")
esptool_py_flash_to_partition(flash vibe_mode "${recovery_output}/factory.bin")
esptool_py_flash_to_partition(flash nvs "${recovery_output}/nvs_initial.bin")
esptool_py_flash_target_image(flash app 0x210000 "${recovery_build_dir}/${recovery_app_bin}")
esptool_py_flash_to_partition(flash ui_apps "${recovery_ui_apps_image}")
esptool_py_flash_to_partition(flash system "${recovery_system_image}")
add_dependencies(flash retained-recovery)

esptool_py_custom_target(update-flash update "app" FILENAME_PREFIX update)
esptool_py_flash_target_image(update-flash app 0x210000 "${recovery_build_dir}/${recovery_app_bin}")
esptool_py_flash_to_partition(update-flash ui_apps "${recovery_ui_apps_image}")
esptool_py_flash_to_partition(update-flash system "${recovery_system_image}")
# Reuse filesystem build dependencies without making update-flash depend on a
# device-writing target. Dependencies are build targets, not flash commands.
get_target_property(recovery_flash_dependencies flash MANUALLY_ADDED_DEPENDENCIES)
add_dependencies(update-flash ${recovery_flash_dependencies})
add_dependencies(app-flash retained-recovery)

file(GENERATE OUTPUT "${recovery_build_dir}/update_flasher_args.json" CONTENT
    "{\n  \"flash_files\": {$<JOIN:$<TARGET_PROPERTY:update-flash,FLASH_FILE>,,>},\n  $<JOIN:$<TARGET_PROPERTY:update-flash,FLASH_ENTRY>,,>\n}\n")

partition_table_add_check_size_target(claw_ota_size
    BINARY_PATH "${recovery_build_dir}/${recovery_app_bin}"
    PARTITION_TYPE app PARTITION_SUBTYPE ota_0 DEPENDS app)
add_dependencies(flash claw_ota_size)
add_dependencies(update-flash claw_ota_size)
add_dependencies(app-flash claw_ota_size)
