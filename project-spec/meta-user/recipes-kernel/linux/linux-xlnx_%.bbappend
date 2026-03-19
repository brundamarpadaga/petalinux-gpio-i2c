FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append = " file://bsp.cfg"
KERNEL_FEATURES:append = " bsp.cfg"
SRC_URI += "file://user_2026-01-11-21-17-00.cfg \
            file://user_2026-01-13-20-13-00.cfg \
            "

