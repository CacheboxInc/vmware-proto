# cva makefile

# note: define CDEV_LIBTASK to get cdevcor.h to expand macros to libtask names

export CC = gcc
export CFLAGS = -Wall -Werror -ggdb -D CDEV_LIBTASK -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64
export LDFLAGS

ifeq ($(ARCH),32)
	CFLAGS += -m32
	LDFLAGS += -m32
else
	a = $(shell getconf LONG_BIT)
	CFLAGS += -m$(a)
	LDFLAGS += -m$(a)
endif

SUBDIRS = libtask rpc iosplitter
DEPEND_DIRS = $(SUBDIRS)
DEPEND_DIRS += tests

.PHONY : subdirs $(SUBDIRS)

subdirs : $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@

clean :
	for dir in $(DEPEND_DIRS); do $(MAKE) -C $$dir clean; done
	rm common/*.o

depend : 
	for dir in $(DEPEND_DIRS); do $(MAKE) -C $$dir depend; done

.PHONY: tests
tests: subdirs
	make -C tests

check: $(SUBDIRS)
	make -C tests
	python ./scripts/run-tests.py

# WARNING --- Do not edit below this line, it is rebuilt by make depend.
#BEGIN_DEPEND
