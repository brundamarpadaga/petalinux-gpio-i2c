FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI += "file://51ntpdate"

do_install:append() {
    if grep -q "CONFIG_UDHCPC=y" ${B}/.config; then
        install -m 0755 ${WORKDIR}/51ntpdate ${D}${sysconfdir}/udhcpc.d/51ntpdate
    fi
}

FILES:${PN}-udhcpc += "${sysconfdir}/udhcpc.d/51ntpdate"
