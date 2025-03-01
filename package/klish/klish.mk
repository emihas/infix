################################################################################
#
# klish
#
################################################################################

KLISH_VERSION = tags/3.0.0
KLISH_SITE = https://src.libcode.org/pkun/klish.git
KLISH_SITE_METHOD = git
KLISH_LICENSE = BSD-3
KLISH_LICENSE_FILES = LICENSE
KLISH_DEPENDENCIES = libxml2
KLISH_INSTALL_STAGING = YES
KLISH_AUTORECONF = YES

KLISH_CONF_OPTS += --with-libxml2

define KLISH_INSTALL_CONFIG
	$(INSTALL) -t $(TARGET_DIR)/etc/klish -D -m 0644 \
		$(@D)/klish.conf $(@D)/klishd.conf

#	klish's default pager settings assumes that less understands
#	all GNU options, so we disable it.
	sed -i -e 's/#UsePager=y/UsePager=n/' \
		$(TARGET_DIR)/etc/klish/klish.conf
endef
KLISH_POST_INSTALL_TARGET_HOOKS += KLISH_INSTALL_CONFIG

ifeq ($(BR2_PACKAGE_KLISH_DEFAULT_XML),y)
define KLISH_INSTALL_XML
	$(INSTALL) -t $(TARGET_DIR)/etc/klish -D -m 0644 \
		$(BR2_EXTERNAL_INFIX_PATH)/package/klish/default.xml
endef
KLISH_POST_INSTALL_TARGET_HOOKS += KLISH_INSTALL_XML
endif

define KLISH_INSTALL_FINIT_SVC
	$(INSTALL) -D -m 0644 $(BR2_EXTERNAL_INFIX_PATH)/package/klish/klish.svc \
		$(FINIT_D)/available/klish.conf
	$(INSTALL) -d -m 0755 $(FINIT_D)/enabled
	ln -sf ../available/klish.conf $(FINIT_D)/enabled/klish.conf
endef

KLISH_POST_INSTALL_TARGET_HOOKS += KLISH_INSTALL_FINIT_SVC

$(eval $(autotools-package))
