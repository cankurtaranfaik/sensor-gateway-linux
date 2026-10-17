################################################################################
#
# telemd
#
################################################################################

TELEMD_VERSION = 1.0
TELEMD_SITE = $(BR2_EXTERNAL_SENSGW_PATH)/package/telemd/src
TELEMD_SITE_METHOD = local
TELEMD_LICENSE = MIT

# the sample struct lives next to the driver
TELEMD_CFLAGS = $(TARGET_CFLAGS) -I$(BR2_EXTERNAL_SENSGW_PATH)/package/vimu/src

define TELEMD_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) \
		CFLAGS="$(TELEMD_CFLAGS)" -C $(@D)
endef

define TELEMD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/telemd $(TARGET_DIR)/usr/bin/telemd
endef

$(eval $(generic-package))
