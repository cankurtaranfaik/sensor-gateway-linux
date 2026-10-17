################################################################################
#
# vimu
#
################################################################################

VIMU_VERSION = 1.0
VIMU_SITE = $(BR2_EXTERNAL_SENSGW_PATH)/package/vimu/src
VIMU_SITE_METHOD = local
VIMU_LICENSE = GPL-2.0

$(eval $(kernel-module))
$(eval $(generic-package))
