PHILICS_TELEMETRY_VERSION = 1.0
PHILICS_TELEMETRY_SITE = $(BR2_EXTERNAL_QUT_PATH)/package/philics-telemetry/src
PHILICS_TELEMETRY_SITE_METHOD = local

define PHILICS_TELEMETRY_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) CC="$(TARGET_CC)" -C $(@D)
endef

define PHILICS_TELEMETRY_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/philics-telemetry $(TARGET_DIR)/usr/bin/philics-telemetry
	$(INSTALL) -D -m 0755 $(PHILICS_TELEMETRY_PKGDIR)/philics-runinc $(TARGET_DIR)/usr/bin/philics-runinc
endef

define PHILICS_TELEMETRY_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(PHILICS_TELEMETRY_PKGDIR)/S99philics \
		$(TARGET_DIR)/etc/init.d/S99philics
endef

$(eval $(generic-package))