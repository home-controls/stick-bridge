idf_component_register(
    SRCS "main.c" "settings.c" "net.c" "dongle.c" "bridge.c" "ezsp_decode.c" "stats.c" "web.c" "logbuf.c"
         "sysinfo.c" "watchdog.c" "fwcheck.c" "updater.c" "ncpflash.c" "ncpflash_core.c"
         "logstore.c" "logstore_core.c" "sdlog.c" "txpace_core.c" "usbser.c" "usbser_core.c" "serial_baud.c" "dongles.c" "protodetect.c" "radio_core.c" "tasks.c" "fwcat.c" "fwcat_core.c" "xflash.c" "unzip_core.c" "ihex_core.c" "tiflash_core.c" "gcfflash_core.c" "nrfdfu_core.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_eth esp_netif esp_event nvs_flash esp_timer esp_http_server app_update
                  esp_app_format esp_partition esp_driver_gpio esp_driver_tsens lwip usb esp_ringbuf
                  json mbedtls esp_http_client esp-tls spi_flash fatfs sdmmc esp_driver_sdmmc vfs)

# The dashboard: one self-contained HTML file, gzipped at build time and served from flash.
idf_build_get_property(python PYTHON)
set(WEB_SRC "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html")
set(WEB_GZ "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")
add_custom_command(
    OUTPUT "${WEB_GZ}"
    COMMAND "${python}" -c "import gzip,sys; d=open(sys.argv[1],'rb').read(); open(sys.argv[2],'wb').write(gzip.compress(d,9,mtime=0))" "${WEB_SRC}" "${WEB_GZ}"
    DEPENDS "${WEB_SRC}"
    VERBATIM)
add_custom_target(web_gz DEPENDS "${WEB_GZ}")
add_dependencies(${COMPONENT_LIB} web_gz)
target_add_binary_data(${COMPONENT_LIB} "${WEB_GZ}" BINARY)
