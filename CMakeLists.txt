target_include_directories(app PRIVATE include)
target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_PMW_CPI app PRIVATE src/behaviors/behavior_pmw_cpi.c)

# Pantalla del dongle: apagar el backlight en reposo. Solo con brillo fijo; con
# el sensor de luz su hilo pisaria el valor (ver el comentario del fichero).
if(CONFIG_SHIELD_PROSPECTOR_ADAPTER AND NOT CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR)
  target_sources(app PRIVATE src/display_idle_backlight.c)
endif()
