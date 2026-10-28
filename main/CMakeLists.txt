idf_component_register(
    SRCS "main.c"
         "pocket_pal_app.c"
         "pocket_pal_audio.c"
         "pocket_pal_model.c"
         "pocket_pal_store.c"
         "pocket_pal_view.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_timer nvs_flash
)

target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/pocket_pal_font_16.c"
)
