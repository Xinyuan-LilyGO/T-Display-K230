################################################################################
#
# rtl8723ds-bt
#
################################################################################

RTL8723DS_BT_VERSION = 14cedf3a9fec1aa8c500fa52f3e3acc433cbcf08
RTL8723DS_BT_SITE = $(call github,wsyco,RTL8723DS_BT_Linux,$(RTL8723DS_BT_VERSION))
RTL8723DS_BT_LICENSE = PROPRIETARY

define RTL8723DS_BT_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) -C $(@D)/rtk_hciattach
endef

define RTL8723DS_BT_INSTALL_TARGET_CMDS
	$(INSTALL) -m 644 -D $(@D)/8723D/rtl8723d_fw \
		$(TARGET_DIR)/lib/firmware/rtl_bt/rtl8723ds_fw.bin
	$(INSTALL) -m 644 -D $(@D)/8723D/rtl8723d_config_1000000_noflow \
		$(TARGET_DIR)/lib/firmware/rtl_bt/rtl8723ds_config.bin
	$(INSTALL) -m 644 -D $(@D)/8723D/rtl8723d_fw \
		$(TARGET_DIR)/lib/firmware/rtlbt/rtl8723d_fw
	$(INSTALL) -m 644 -D $(@D)/8723D/rtl8723d_config_1000000_noflow \
		$(TARGET_DIR)/lib/firmware/rtlbt/rtl8723d_config
	$(INSTALL) -m 644 -D $(@D)/8723D/rtl8723d_config \
		$(TARGET_DIR)/lib/firmware/rtlbt/rtl8723d_config_default
	$(INSTALL) -m 644 -D $(@D)/8723D/rtl8723d_config_1000000_noflow \
		$(TARGET_DIR)/lib/firmware/rtlbt/rtl8723d_config_1000000_noflow
	$(INSTALL) -m 755 -D $(@D)/rtk_hciattach/rtk_hciattach \
		$(TARGET_DIR)/usr/sbin/rtk_hciattach
endef

$(eval $(generic-package))
