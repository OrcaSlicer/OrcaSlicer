# async_mqtt is header-only: OrcaMqttConnection uses only its I/O-independent MQTT 3.1.1
# protocol layer, so the headers are installed as they are, with no configure or build step.
ExternalProject_Add(dep_AsyncMqtt
    EXCLUDE_FROM_ALL ON
    URL https://github.com/redboltz/async_mqtt/archive/refs/tags/10.3.1.tar.gz
    URL_HASH SHA256=0d4796534d1db9fcf948393d46aae9aa636aab3a2eb44843868358623363ae1c
    DOWNLOAD_DIR ${DEP_DOWNLOAD_DIR}/AsyncMqtt
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ${CMAKE_COMMAND} -E copy_directory "<SOURCE_DIR>/include/async_mqtt" "${DESTDIR}/include/async_mqtt"
)
