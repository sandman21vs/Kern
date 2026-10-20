if(CONFIG_KERN_NFC)
    set(srcs "src/nfc.c" "src/nfc_record.c" "src/pcd.c" "src/picc.c")
    if(CONFIG_KERN_NFC_WS1850S)
        list(APPEND srcs "src/pcd_ws1850s.c")
    endif()
    if(CONFIG_KERN_NFC_PN532)
        list(APPEND srcs "src/pcd_pn532.c")
    endif()
    idf_component_register(
        SRCS ${srcs}
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
