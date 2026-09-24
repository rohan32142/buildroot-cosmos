################################################################################
#
# pldrift
#
################################################################################

PLDRIFT_VERSION = 1.0
PLDRIFT_SITE = $(BR2_EXTERNAL_QUT_PATH)/package/pldrift/src
PLDRIFT_SITE_METHOD = local

define PLDRIFT_BUILD_CMDS
    $(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -O2 \
        -o $(@D)/pldrift $(@D)/pldrift.c
endef

define PLDRIFT_INSTALL_TARGET_CMDS
    $(INSTALL) -D -m 0755 $(@D)/pldrift $(TARGET_DIR)/usr/bin/pldrift
    $(INSTALL) -D -m 0755 $(@D)/ocmwatch.sh $(TARGET_DIR)/usr/bin/ocmwatch
endef

$(eval $(generic-package))