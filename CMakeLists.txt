target_include_directories(app PRIVATE include)
target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_PMW_CPI app PRIVATE src/behaviors/behavior_pmw_cpi.c)

# Pantalla del dongle: apagar el backlight en reposo. Con el sensor de luz lo
# hace display_als_backlight.c, que ademas ajusta el brillo; sin el, brillo
# fijo con display_idle_backlight.c. Con el sensor del modulo Prospector no se
# compila ninguno: su hilo pisaria el valor (ver el comentario de los ficheros).
if(CONFIG_DONGLE_ALS)
  target_sources(app PRIVATE src/display_als_backlight.c)
elseif(CONFIG_SHIELD_PROSPECTOR_ADAPTER AND NOT CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR
       AND NOT CONFIG_DONGLE_RTC)
  target_sources(app PRIVATE src/display_idle_backlight.c)
endif()

# Reloj DS3231 del dongle y &rtc_adj para ponerlo en hora. Con el reloj, la
# pantalla no se apaga en reposo (pasa a modo reloj), asi que sin sensor de luz
# no hace falta display_idle_backlight.c.
target_sources_ifdef(CONFIG_DONGLE_RTC app PRIVATE src/dongle_rtc.c src/behaviors/behavior_rtc_adjust.c
                     src/fonts/Clock_Time_64.c src/fonts/Clock_Date_30.c)
target_sources_ifdef(CONFIG_DONGLE_TOUCH app PRIVATE src/dongle_touch.c)
