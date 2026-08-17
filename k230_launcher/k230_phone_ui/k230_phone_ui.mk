################################################################################
#
# k230_phone_ui
#
################################################################################

K230_PHONE_UI_SITE = $(realpath $(TOPDIR))/package/k230_phone_ui/src
K230_PHONE_UI_SITE_METHOD = local
K230_PHONE_UI_INSTALL_TARGET = YES
K230_PHONE_UI_SUPPORTS_IN_SOURCE_BUILD = NO
K230_PHONE_UI_DEPENDENCIES += lvgl libdrm libevdev vvcam libgpiod2 openssl opus libcodec2

$(eval $(cmake-package))
