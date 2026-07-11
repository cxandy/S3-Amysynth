idf_component_register(
    SRCS "project_fs.c" "project_tlv.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_partition joltwallet__littlefs
)
