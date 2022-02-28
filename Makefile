SRCDIR ?= /opt/fpp/src
include $(SRCDIR)/makefiles/common/setup.mk
include $(SRCDIR)/makefiles/platform/*.mk

all: libfpp-smpte.$(SHLIB_EXT)
debug: all

CFLAGS+=-I.
OBJECTS_fpp_smpte_so += src/FPPSMPTE.o
LIBS_fpp_smpte_so += -L$(SRCDIR) -lfpp -ljsoncpp -lhttpserver -lltc -lsdl2
CXXFLAGS_src/FPPSMPTE.o += -I$(SRCDIR)

ifeq '$(ARCH)' 'OSX'
LTCHEADER=$(HOMEBREW)/include/ltc.h
$(LTCHEADER):
	brew install libltc
else
LTCHEADER=/usr/include/ltc.h
$(LTCHEADER):
	sudo apt-get install -y libltc-dev
endif


%.o: %.cpp Makefile $(LTCHEADER)
	$(CCACHE) $(CC) $(CFLAGS) $(CXXFLAGS) $(CXXFLAGS_$@) -c $< -o $@

libfpp-smpte.$(SHLIB_EXT): $(OBJECTS_fpp_smpte_so) $(SRCDIR)/libfpp.$(SHLIB_EXT)
	$(CCACHE) $(CC) -shared $(CFLAGS_$@) $(OBJECTS_fpp_smpte_so) $(LIBS_fpp_smpte_so) $(LDFLAGS) -o $@

clean:
	rm -f libfpp-smpte.so $(OBJECTS_fpp_smpte_so)

