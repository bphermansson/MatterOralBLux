idf_component_register(SRCS              "app_main.cpp"
                                            "src/brush_handler.c"
                                            "src/telegram.c"
                                            "../components/veml7700_handler.cpp"
                       PRIV_INCLUDE_DIRS "."  "include" "../components/oralb_ble/include" "${ESP_MATTER_PATH}/examples/common/utils"
                       EMBED_TXTFILES    "telegram_certificate.pem")

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
