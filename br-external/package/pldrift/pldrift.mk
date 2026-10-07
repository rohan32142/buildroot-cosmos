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
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -O2 \
		-o $(@D)/adcmon $(@D)/adcmon.c -lm
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -O2 \
		-o $(@D)/ocmsoak $(@D)/ocmsoak.c -lm
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -O2 \
		-o $(@D)/adcburst $(@D)/adcburst.c -lm
endef

define PLDRIFT_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/pldrift $(TARGET_DIR)/usr/bin/pldrift
	$(INSTALL) -D -m 0755 $(@D)/adcmon $(TARGET_DIR)/usr/bin/adcmon
	$(INSTALL) -D -m 0755 $(@D)/ocmsoak $(TARGET_DIR)/usr/bin/ocmsoak
	$(INSTALL) -D -m 0755 $(@D)/adcburst $(TARGET_DIR)/usr/bin/adcburst
	$(INSTALL) -D -m 0755 $(@D)/ocmwatch.sh $(TARGET_DIR)/usr/bin/ocmwatch
	$(INSTALL) -D -m 0755 $(@D)/labup.sh $(TARGET_DIR)/usr/bin/labup
	$(INSTALL) -D -m 0755 $(@D)/soakrun.sh $(TARGET_DIR)/usr/bin/soakrun
endef

$(eval $(generic-package))