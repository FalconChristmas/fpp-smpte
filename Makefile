include /opt/fpp/src/makefiles/common/setup.mk
include /opt/fpp/src/makefiles/platform/*.mk

all: libfpp-smpte.so
debug: all

CFLAGS+=-I.
OBJECTS_fpp_smpte_so += src/FPPSMPTE.o
LIBS_fpp_smpte_so += -L/opt/fpp/src -lfpp -lltc
CXXFLAGS_src/FPPSMPTE.o += -I/opt/fpp/src


%.o: %.cpp Makefile /usr/include/ltc.h
	$(CCACHE) $(CC) $(CFLAGS) $(CXXFLAGS) $(CXXFLAGS_$@) -c $< -o $@

libfpp-smpte.so: $(OBJECTS_fpp_smpte_so) /opt/fpp/src/libfpp.so
	$(CCACHE) $(CC) -shared $(CFLAGS_$@) $(OBJECTS_fpp_smpte_so) $(LIBS_fpp_smpte_so) $(LDFLAGS) -o $@

clean:
	rm -f libfpp-smpte.so $(OBJECTS_fpp_smpte_so)

/usr/include/ltc.h:
	sudo apt-get install -y libltc-dev
