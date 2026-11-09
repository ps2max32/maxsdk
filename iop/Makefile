# maxsdk's IOP modules, laid out as ps2sdk's iop/: <group>/<module>/{src,include}.
# What the EE and the IOP share is in ../common/include.
#
#   make [OUT=<dir>]    <dir>/*.irx

SUBDIRS = dev9/mxdev9 dev9/mxeth dev9/mxata tcpip/mxnet tcpip/mxfast tcpip/mxsock fs/mxio usb/usbmass_bd usb/mxpad sound/mxsnd

all: $(SUBDIRS)
$(SUBDIRS):
	$(MAKE) -C $@

.PHONY: all $(SUBDIRS)
