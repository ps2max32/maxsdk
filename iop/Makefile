# maxsdk's IOP modules, laid out as ps2sdk's iop/: <group>/<module>/{src,include}.
# What the EE and the IOP share is in ../common/include.
#
#   make [OUT=<dir>]    <dir>/*.irx

SUBDIRS = tcpip/mxnet tcpip/mxsock fs/mxio

all: $(SUBDIRS)
$(SUBDIRS):
	$(MAKE) -C $@

.PHONY: all $(SUBDIRS)
