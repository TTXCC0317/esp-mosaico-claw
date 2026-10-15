# Register one complete installation and one application-only update. Never
# include the retained test image in an application-only update.
idf_build_get_property(recovery_build_dir BUILD_DIR)
idf_build_get_property(recovery_app_name EXECUTABLE_NAME)
set(recovery_app_bin "${recovery_app_name}.bin")
set(ESP_MOSAICO_VIBE_MODE_BUNDLE "${CMAKE_SOURCE_DIR}/third-party/esp-mosaico-utils/esp-mosaico-vibe-mode/firmware/vibe-mode/prebuilt/recovery"
    CACHE PATH "Validated Vibe Mode bundle to include in the installation")
set(recovery_bundle "${ESP_MOSAICO_VIBE_MODE_BUNDLE}")
set(recovery_output "${recovery_build_dir}/recovery/bundle")
set(recovery_claw_otadata "${recovery_build_dir}/recovery/ota_data_claw.bin")
if(NOT EXISTS "${recovery_bundle}/manifest.json")
    message(FATAL_ERROR "Vibe Mode bundle missing: ${recovery_bundle}; initialize utils or set ESP_MOSAICO_VIBE_MODE_BUNDLE to a validated bundle")
endif()

# Current-source bundles use vibe-mode.bin; reviewed older bundles use factory.bin.
file(READ "${recovery_bundle}/manifest.json" recovery_manifest)
string(JSON recovery_image GET "${recovery_manifest}" images recovery file)
if(NOT recovery_image STREQUAL "vibe-mode.bin" AND NOT recovery_image STREQUAL "factory.bin")
    message(FATAL_ERROR "Unexpected Vibe Mode image: ${recovery_image}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${recovery_bundle}/manifest.json")

# Blank otadata selects Claw instead of the upstream Vibe Mode marker.
string(ASCII 255 recovery_erased_byte)
string(REPEAT "${recovery_erased_byte}" 8192 recovery_otadata)
file(GENERATE OUTPUT "${recovery_claw_otadata}" CONTENT "${recovery_otadata}")

idf_build_get_property(recovery_python PYTHON)
idf_build_get_property(recovery_target IDF_TARGET)
set(recovery_tool "${CMAKE_SOURCE_DIR}/third-party/esp-mosaico-utils/esp-mosaico-vibe-mode/firmware/vibe-mode/tools/prepare_recovery.py")
partition_table_get_partition_info(recovery_offset "--partition-name vibe_mode" "offset")
partition_table_get_partition_info(recovery_size "--partition-name vibe_mode" "size")
partition_table_get_partition_info(recovery_normal_offset "--partition-name main_app" "offset")
partition_table_get_partition_info(recovery_ota_offset "--partition-name otadata" "offset")
set(recovery_secure_boot 0)
set(recovery_flash_encryption 0)
if(CONFIG_SECURE_BOOT)
    set(recovery_secure_boot 1)
endif()
if(CONFIG_SECURE_FLASH_ENC_ENABLED)
    set(recovery_flash_encryption 1)
endif()

# Validate hashes and the product layout on every build before staging flash inputs.
add_custom_target(retained-recovery ALL
    COMMAND "${recovery_python}" "${recovery_tool}"
        --bootloader "${recovery_bundle}/bootloader.bin"
        --partition-table "${recovery_bundle}/partition-table.bin"
        --ota-data "${recovery_bundle}/ota_data_initial.bin"
        --recovery "${recovery_bundle}/${recovery_image}"
        --manifest "${recovery_bundle}/manifest.json"
        --target "${recovery_target}"
        --bootloader-offset "${CONFIG_BOOTLOADER_OFFSET_IN_FLASH}"
        --partition-table-offset "${CONFIG_PARTITION_TABLE_OFFSET}"
        --ota-data-offset "${recovery_ota_offset}"
        --recovery-offset "${recovery_offset}" --recovery-size "${recovery_size}"
        --normal-offset "${recovery_normal_offset}"
        --secure-boot "${recovery_secure_boot}" --flash-encryption "${recovery_flash_encryption}"
        --output-dir "${recovery_output}"
    BYPRODUCTS "${recovery_output}/bootloader.bin" "${recovery_output}/${recovery_image}"
        "${recovery_output}/manifest.json" "${recovery_output}/partition-table.bin"
        "${recovery_output}/ota_data_initial.bin"
    DEPENDS partition_table_bin "${recovery_tool}"
    COMMENT "Validating and staging the retained Vibe Mode bundle"
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

# Preserve application NVS and Vibe Mode sysmeta when reinstalling firmware.
esptool_py_flash_target_image(flash bootloader 0x2000 "${recovery_output}/bootloader.bin")
esptool_py_flash_target_image(flash partition-table 0x8000 "${recovery_build_dir}/partition_table/partition-table.bin")
esptool_py_flash_to_partition(flash otadata "${recovery_claw_otadata}")
esptool_py_flash_to_partition(flash vibe_mode "${recovery_output}/${recovery_image}")
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
