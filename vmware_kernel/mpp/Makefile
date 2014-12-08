#
# Makefile
#
# type:
#	make
# to build the module
#
#
# Expected usage is that this Makefile is copied to a new source directory,
# and the variables above the #### line are adjusted appropriately.

CURRENT_DIR := $(shell pwd)
BASENAME := $(COREUTILSBIN)/bin/basename


DEV_PROJECT = mp-plugin-example
DEV_PROJECT_VERSION = 1.0


MODULE_JSON_SCRIPT = $(CURRENT_DIR)/psa-mp-plugin-example.json
MODULE_RULES_JSON_SCRIPT = $(CURRENT_DIR)/psa-mp-plugin-example-rules.json
DEV_VMK_JSON_SCRIPTS = \
	$(MODULE_JSON_SCRIPT) \
	$(MODULE_RULES_JSON_SCRIPT)

# DEV_VIB_BASENAME is set below, must use "=" here.
DESC_NAME=$(DEV_VIB_BASENAME)
DESC_VENDOR_EMAIL=none@vmware.com
DESC_VERSION=$(DEV_PROJECT_VERSION)-1
DESC_VENDOR=vmware
DESC_SUMMARY=PSA Example
DESC_DESCRIPTION=Sample Pluggable Storage Architecture Multipathing module for ESX Server
DESC_RELEASE_DATE=2010-08-25T12:44:41-07:00
DESC_URLS=descriptor-urls.xml
DESC_DEPENDS=
DESC_CONFLICTS=
DESC_REPLACES=
DESC_PROVIDES=descriptor-provides.xml
DESC_MAINT=1
DESC_LIVE_INSTALL_ALLOWED=true
DESC_LIVE_REMOVE_ALLOWED=true
DESC_STATELESS_READY=true

DESC_ACCEPTANCE_LEVEL=accepted
KEYPATH=$(strip $(VMW_VIB_BASE)/testcerts/accepted.key)
CERTPATH=$(strip $(VMW_VIB_BASE)/testcerts/accepted.cert)

BULL_KBURL=http://kb.vmware.com/kb/example.html
BULL_PLATFORMS=bulletin-platforms.xml
BULL_VENDOR_CODE=VMW
RM_VIBID_SED = $(VMW_KMDK_BASE)/vide/tools/rm_vibid.sed

################################################################
# Set build configuration options here.
#
# Variables starting with "DEV_" are reserved for communicating
# build parameters into the core makefiles (in .../samples/common).
#
# Those starting with "VMW_" are reserved for VMware use, and should
# be considered unsupported with no compatibility guarantees unless
# explicitly documented otherwise.
#
# Note that many variables do not currently follow the above
# patterns.  This is to minimize changes required in existing
# partner code.

################
# This controls which type of compiler options to use.  Allowed
# values are "beta" (for debugging) and "release".  The default is
# the type of the ESX build that produced this devkit.
DEV_BUILDTYPE = beta

################
# These are the project-specific C files.  The common include file
# kmdk.inc brought in below uses it as the base for defining
# VMW_ALL_VMK_CFILES.
DEV_VMK_CFILES = mp_plugin_example.c mgmtInterface.c
DEV_VMK_CFILES += queue.c hash.c network.c bufpool.c rpc.c
DEV_UW_CFILES = mgmtInterface.c userProgram.c

################
# Typically, it won't be necessary to modify these settings, but the
# substitutions pattern may need to put the intermediate files in a
# more convenient location.  Note that VMW_ALL_VMK_CFILES is set in
# kmdk.inc, and by using "=" it won't be expanded until we actually
# use $(DEV_$(DEV_BUILDTYPE)_VMK_OBJECTS).  Similarly for ..._UW_....
DEV_$(DEV_BUILDTYPE)_VMK_OBJECTS = \
	$(patsubst %.c,$(DEV_VMK_OBJECT_DIR)/%.o,$(VMW_ALL_VMK_CFILES))

DEV_$(DEV_BUILDTYPE)_UW_OBJECTS = \
	$(patsubst %.c,$(DEV_UW_OBJECT_DIR)/%.o,$(VMW_ALL_UW_CFILES))

################
# Choose a base name for the VIB.  The prefix (here, "vmware-esx")
# should be specific to the partner/developer.
DEV_VIB_BASENAME = vmware-esx-mp-plugin-example

################
# Choose a name for the bulletin.  The prefix (here, "VMW-ESX")
# should be specific to the partner/developer and unique across
# all bundles published by that partner/developer.  Using VMW_VERSION
# (which is set in kmdk.inc) because the more-obvious DEV_ESX_VERSION
# or DESC_ESX_VERSION may contain characters that are not legal in
# a bulletin id string.
VIB_BULLETIN_ID = VMW-ESX-$(VMW_VERSION)-$(DEV_PROJECT)-$(DEV_PROJECT_VERSION)

################
# If creating a standalone userworld application, as opposed to a
# shared object that gets loaded by some other program, uncomment
# the following assignment.  The value needs to be empty or non-
# empty.  If the latter, any content may be used.
DEV_APP = 1

################
# Extra compiler and preprocessor flags based on kind of build
# (release or beta).  These default to being empty.
# DEV_beta_COMPILEFLAGS = -Dmy_debug_flag
# DEV_release_COMPILEFLAGS = -Dmy_optimized_flag
#
# For flags common to all build types, you can use:
# DEV_$(DEV_BUILDTYPE)_COMPILEFLAGS += ...

# Note: You might want to change the license string

DEV_$(DEV_BUILDTYPE)_COMPILEFLAGS += -DVMK_DEVKIT_HAS_API_VMKAPI_SCSI
DEV_$(DEV_BUILDTYPE)_COMPILEFLAGS += -DVMK_DEVKIT_HAS_API_VMKAPI_MPP
DEV_$(DEV_BUILDTYPE)_COMPILEFLAGS += -D_MODULE_LICENSE_="\"VMware, Inc.\""

################
# Define here any additional userworld libraries required by
# uncommenting the appropriate variable(s) and modifying as needed.
# If the library is not included in the devkit distribution, include a
# -L /some/path parameter before the -lname parameter so that the
# linker can find it.  The *PRE* variables are included as the first
# library directives.  The *POST* variables are included as the last
# such directives.
#
# The default values are empty.

# DEV_release_PRE_LIBS =
# DEV_beta_PRE_LIBS = -L /acme/print/lib -lprintwatch

# The third assignment below is semantically equivalent to the
# preceding two assignments, since we only do one type of build at
# a time.  See the gnumake documentation for details.

# DEV_release_POST_LIBS = -lm
# DEV_beta_POST_LIBS = -lm
# or
# DEV_$(DEV_BUILDTYPE)_POST_LIBS = -lm

################
# If the vdsPublic library needs to be included in the compilation,
# uncomment the following assignment.  Currently, that library only
# comes as part of the "kmdk-devtools-vds" devkit, so it and any
# other devkit components it depends on will need to be installed
# on the same development host as this devkit.  The value must be
# empty for not needing vdsPublic, and non-empty for needing it.
# The actual content is not examined.

#DEV_NEEDVDSLIB = 1

################################################################
# Nothing beyond this point should require modification.

BUILD_DIR = $(CURRENT_DIR)/build
SCRATCH_DIR = $(BUILD_DIR)/scratch
VIB_DIR = $(BUILD_DIR)/vib
BUNDLE_DIR = $(BUILD_DIR)/bundle

# STAGE_DIR is the base directory that will be populated exactly as how
# the delivered files are to be installed.  It corresponds to the
# target host's root (/) directory.
STAGE_DIR = $(BUILD_DIR)/stage/$(DEV_BUILDTYPE)

DEV_VMK_OBJECT_DIR = $(BUILD_DIR)/$(DEV_BUILDTYPE)/vmk
DEV_VMK_BIN = $(DEV_VMK_OBJECT_DIR)/$(DEV_PROJECT)
DEV_VMK_BIN_STRIPPED = $(DEV_VMK_BIN)-stripped

DEV_VIB_XML = $(CURRENT_DIR)/descriptor.xml
DEV_BULLETIN_XML = $(CURRENT_DIR)/bulletin.xml

DEV_UW_OBJECT_DIR = $(BUILD_DIR)/$(DEV_BUILDTYPE)/usw
DEV_UW_BIN = $(DEV_UW_OBJECT_DIR)/$(DEV_PROJECT)


# Note that this hard-codes the prefix path at installation
# time.  This allows for having multiple different versions
# of the devkit installed and usable simultaneously.

include /opt/vmware/kmdk-5.5.0-1331820/samples/common/kmdk.inc

.PHONY: $(DEV_VMK_BIN) $(VMW_TYPES) clean

# The kmdk.inc file included above defines VMW_ALL_VMK_CFILES using
# DEV_VMK_CFILES as a starting point.  Similarly for VMW_ALL_UW_CFILES
# and DEV_UW_CFILES.

all: vib

build-prep:
	@echo "**  Preparing $(DEV_PROJECT) build tree"
	$(MKDIR) -p $(DEV_VMK_OBJECT_DIR) $(DEV_UW_OBJECT_DIR)

# Targets "release" and "beta" are defined by kmdk.inc to build all
# the binaries with the appropriate flags, but not do any packaging
# (no vib, no offline bundle).  If you want to produce the binaries
# with whatever build type is the default, you can also use the
# target "binaries".

all-workbench: binaries package
include all-or-objs.inc

vib: binaries minimal-descriptor fill-bulletin package

package:
	@echo "**  Staging content of $(DEV_VIB_BASENAME) vib"
	$(RM) -rf $(STAGE_DIR)
    ifneq ($(strip $(DEV_VMK_BIN)),)
	$(MKDIR) -p $(STAGE_DIR)/usr/lib/vmware/vmkmod
	$(CP) $(DEV_VMK_BIN_STRIPPED) $(STAGE_DIR)/usr/lib/vmware/vmkmod/$(DEV_PROJECT)
    endif
    ifneq ($(strip $(DEV_UW_BIN)),)
	$(MKDIR) -p $(STAGE_DIR)/$(VMW_UW_BASE_DIR)/sbin
	$(CP) $(DEV_UW_BIN_STRIPPED) $(STAGE_DIR)/$(VMW_UW_BASE_DIR)/sbin/$(notdir $(DEV_UW_BIN))
    endif
    ifneq ($(strip $(DEV_VMK_JSON_SCRIPTS)),)
	$(MKDIR) -p $(STAGE_DIR)/usr/libexec/jumpstart/plugins
	$(SED) -e "s/-m mp-plugin-example/-m $(DEV_PROJECT)/" $(MODULE_JSON_SCRIPT) > $(STAGE_DIR)/usr/libexec/jumpstart/plugins/`$(BASENAME) $(MODULE_JSON_SCRIPT)`
	$(CP) $(MODULE_RULES_JSON_SCRIPT) $(STAGE_DIR)/usr/libexec/jumpstart/plugins
    endif
	$(SED) -i -f $(RM_VIBID_SED) $(DEV_BULLETIN_XML)
	$(MAKE) -f Makefile DEV_ESX_VERSION=5.5.0 vib-bundle-level

include build-bundle.inc

clean:
	$(RM) -rf $(BUILD_DIR)

really-clean: clean
	$(RM) -f $(DEV_VIB_XML) $(DEV_BULLETIN_XML) vendor-index.xml
