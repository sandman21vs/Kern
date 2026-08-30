if(CONFIG_KERN_NFC)
    idf_component_register(
        SRCS "src/nfc.c" "src/nfc_record.c" "src/pcd_ws1850s.c" "src/picc.c"
        INCLUDE_DIRS "include"
        REQUIRES esp_driver_i2c
        PRIV_REQUIRES esp_timer
    )
else()
    idf_component_register(
        INCLUDE_DIRS "include"
        REQUIRES esp_driver_i2c
    )
endif()
