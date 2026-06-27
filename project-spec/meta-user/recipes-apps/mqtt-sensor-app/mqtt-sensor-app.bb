SUMMARY = "MQTT sensor publisher — publishes BME280 data to HiveMQ Cloud over TLS"
SECTION = "PETALINUX/apps"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://mqtt-sensor-app.c \
           file://Makefile \
           file://mqtt-sensor.conf"

S = "${WORKDIR}"

DEPENDS = "paho-mqtt-c"
RDEPENDS:${PN} = "paho-mqtt-c"

do_compile() {
    oe_runmake
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 mqtt-sensor-app ${D}${bindir}

    install -d ${D}${sysconfdir}
    install -m 0600 ${WORKDIR}/mqtt-sensor.conf ${D}${sysconfdir}/mqtt-sensor.conf
}
