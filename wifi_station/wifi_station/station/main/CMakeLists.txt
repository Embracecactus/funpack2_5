idf_component_register(
    SRC_DIRS
        "."
      

    INCLUDE_DIRS
        "."
        "include"
    EMBED_TXTFILES howsmyssl_com_root_cert.pem
                    postman_root_cert.pem
        )

target_compile_definitions(${COMPONENT_TARGET} PRIVATE "-D RMAKER_DEMO_PROJECT_NAME=\"${CMAKE_PROJECT_NAME}\"")

#spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)

add_definitions(-w)

set(voice_data_image ${PROJECT_DIR}/../components/esp-sr/esp-tts/esp_tts_chinese/esp_tts_voice_data_xiaole.dat)
add_custom_target(voice_data ALL DEPENDS ${voice_data_image})
add_dependencies(flash voice_data)

partition_table_get_partition_info(size "--partition-name voice_data" "size")
partition_table_get_partition_info(offset "--partition-name voice_data" "offset")

if("${size}" AND "${offset}")
    esptool_py_flash_to_partition(flash "voice_data" "${voice_data_image}")
else()
    set(message "Failed to find model in partition table file"
                "Please add a line(Name=voice_data, Type=data, Size=3890K) to the partition file.")
endif()