# riemannfeeder Makefile

SRC = src/riemannfeeder.c
PROTOBUF = protobuf
DEP_PROTOBUF = lib/libprotoc.a
PROTOBUFC = protobuf-c
DEP_PROTOBUFC = lib/libprotobuf-c.a
RIEMANNCLIENT = riemann-c-client
DEP_RIEMANNCLIENT = lib/libriemann-client.a

# fallback to gcc when $CC is not in $PATH.
CC := $(shell sh -c 'type $(CC) >/dev/null 2>/dev/null && echo $(CC) || echo gcc')
# for recursive make
ifeq ($(MAKE),)
	MAKE := make
endif
OPTIMIZATION ?= -O3
DEBUG ?= -g -ggdb
REAL_CFLAGS = $(OPTIMIZATION) -fPIC -I/usr/include/naemon -I/usr/lib/x86_64-linux-gnu/glib-2.0/include -I/usr/include/glib-2.0 $(CFLAGS) $(DEBUG) $(ARCH) -DHAVE_CONFIG_H -DNSCORE -DVERSION='"$(RIEMANNFEEDER_VERSION)"' 

# get version from ENV or git
ifndef VERSION 
	RIEMANNFEEDER_VERSION := $(shell git describe --abbrev=7 --dirty --always --tags)
else
	RIEMANNFEEDER_VERSION := $(VERSION)
endif

RF = riemannfeeder-$(RIEMANNFEEDER_VERSION).o

all: $(RF)

# dep:
$(DEP_PROTOBUF):
	if [ ! -d $(PROTOBUF) ] ; then git clone https://github.com/google/protobuf.git $(PROTOBUF) ; fi
	# v3.0.0-beta-3.3 is known to work, but I prefer not to pin it to an old version
	#git checkout v3.0.0-beta-3.3
	cd $(PROTOBUF) ; ./autogen.sh ; PKG_CONFIG_PATH=$(CURDIR)/lib/pkgconfig CFLAGS=-fPIC CXXFLAGS=-fPIC ./configure --with-pic --enable-static --prefix=$(CURDIR)
	$(MAKE) -C $(PROTOBUF) install

$(DEP_PROTOBUFC): $(DEP_PROTOBUF)
	if [ ! -d $(PROTOBUFC) ] ; then git clone https://github.com/protobuf-c/protobuf-c.git $(PROTOBUFC) ; fi
	# v1.2.1 is known to work, but I prefer not to pin it to an old version
	#git checkout v1.2.1
	cd $(PROTOBUFC) ; ./autogen.sh ; PKG_CONFIG_PATH=$(CURDIR)/lib/pkgconfig CFLAGS=-fPIC CXXFLAGS=-fPIC ./configure --with-pic --enable-static --prefix=$(CURDIR)
	$(MAKE) -C $(PROTOBUFC) install

$(DEP_RIEMANNCLIENT): $(DEP_PROTOBUFC)
	if [ ! -d $(RIEMANNCLIENT) ] ; then git clone git://github.com/algernon/riemann-c-client.git $(RIEMANNCLIENT) ; fi
	# riemann-c-client-1.9.0 is known to work, but I prefer not to pin it to an old version
	#git checkout riemann-c-client-1.9.0
	cd $(RIEMANNCLIENT) ; patch < $(CURDIR)/riemann-c-client.HAVE_VERSIONING.static.patch
	cd $(RIEMANNCLIENT) ; autoreconf -fvi 
	cd $(RIEMANNCLIENT) ; CFLAGS=-fPIC CXXFLAGS=-fPIC PKG_CONFIG_PATH=$(CURDIR)/lib/pkgconfig LD_LIBRARY_PATH=$(CURDIR)/lib ./configure --prefix=$(CURDIR)
	LD_LIBRARY_PATH=$(CURDIR)/lib $(MAKE) -C $(RIEMANNCLIENT) install

# modules:
$(RF): $(SRC) $(DEP_RIEMANNCLIENT)
	$(CC) $(REAL_CFLAGS) -o $@ $< -shared $(DEP_RIEMANNCLIENT) $(DEP_PROTOBUFC)
	strip $@
	ln -sf $@ riemannfeeder-latest.o

clean:
	rm -rf *.o $(DEP_PROTOBUF) $(DEP_PROTOBUFC) $(DEP_RIEMANNCLIENT) $(PROTOBUF) $(PROTOBUFC) $(RIEMANNCLIENT)

.PHONY: all clean dep
