################################################################################
#
# faux
#
################################################################################

FAUX_VERSION = tags/2.1.0
FAUX_SITE = https://src.libcode.org/pkun/faux.git
FAUX_SITE_METHOD = git
FAUX_LICENSE = BSD-3
FAUX_LICENSE_FILES = LICENSE
FAUX_INSTALL_STAGING = YES
FAUX_AUTORECONF = YES

$(eval $(autotools-package))
