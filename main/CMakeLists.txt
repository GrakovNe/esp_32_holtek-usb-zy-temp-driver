execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..
    OUTPUT_VARIABLE GIT_VER
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT GIT_VER)
    set(GIT_VER "dev")
endif()

idf_component_register(SRCS "main.c" "settings.c" "reset.c" "boot_btn.c"
                              "wifi_mgr.c" "captive.c" "web_server.c"
                              "time_mgr.c" "history.c" "history_ring.c"
                              "mqtt_mgr.c"
                              "selftest.c"
                       INCLUDE_DIRS "."
                       REQUIRES zytemp appconf nvs_flash esp_partition
                              esp_driver_gpio esp_driver_tsens esp_hw_support
                              esp_system heap esp_wifi esp_netif
                              esp_event esp_timer esp_http_server json
                              spi_flash mqtt esp-tls)

target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_VER="${GIT_VER}")

if(APP_SELFTEST_POST_WIFI)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SELFTEST_POST_WIFI)
endif()

if(APP_SELFTEST_NTP)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SELFTEST_NTP)
endif()

if(APP_SELFTEST_FACTORY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SELFTEST_FACTORY)
endif()

if(APP_SELFTEST_REBOOT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SELFTEST_REBOOT)
endif()

if(APP_SELFTEST_FAST_HISTORY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
                               SELFTEST_HISTORY
                               HIST_WINDOW_S=30 HIST_SAMPLE_MS=1000)
endif()

if(APP_SELFTEST_MQTT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SELFTEST_MQTT)
endif()

if(APP_SELFTEST_WAIT_FALLBACK)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
                               SELFTEST_WAIT_FALLBACK)
endif()

find_program(GZIP_EXECUTABLE gzip REQUIRED)
set(WEB_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../web")
set(WEB_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen_web")

foreach(page setup main info)
    set(sym "${page}_html_gz")
    add_custom_command(
        OUTPUT ${WEB_GEN_DIR}/${page}.html.gz
        COMMAND ${CMAKE_COMMAND} -E make_directory ${WEB_GEN_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy ${WEB_SRC_DIR}/${page}.html
                ${WEB_GEN_DIR}/${page}.html
        COMMAND ${GZIP_EXECUTABLE} -9 -n -f ${WEB_GEN_DIR}/${page}.html
        DEPENDS ${WEB_SRC_DIR}/${page}.html)
    add_custom_command(
        OUTPUT ${WEB_GEN_DIR}/${page}_html.h
        COMMAND ${CMAKE_COMMAND}
                -DBIN=${WEB_GEN_DIR}/${page}.html.gz
                -DOUT=${WEB_GEN_DIR}/${page}_html.h
                -DSYM=${sym}
                -P ${CMAKE_CURRENT_LIST_DIR}/../cmake/bin2header.cmake
        DEPENDS ${WEB_GEN_DIR}/${page}.html.gz
                ${CMAKE_CURRENT_LIST_DIR}/../cmake/bin2header.cmake)
    list(APPEND WEB_GEN_HEADERS ${WEB_GEN_DIR}/${page}_html.h)
endforeach()

add_custom_target(web_assets DEPENDS ${WEB_GEN_HEADERS})
add_dependencies(${COMPONENT_LIB} web_assets)
target_include_directories(${COMPONENT_LIB} PRIVATE ${WEB_GEN_DIR})
