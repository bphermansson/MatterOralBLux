idf_component_register(
    SRCS "oralb_ble.c" "advertisement_message.c"
    INCLUDE_DIRS "include"
    REQUIRES bt oralb_proto
)
