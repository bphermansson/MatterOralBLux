idf_component_register(SRC_DIRS          "."  "src"
                       PRIV_INCLUDE_DIRS "."  "include" "${ESP_MATTER_PATH}/examples/common/utils"
                       EMBED_TXTFILES    "telegram_certificate.pem")

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
