CFLAGS += -g -Wall -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -I.. -I../include
LDFLAGS += -L../libtask -L../rpc
LDLIBS = -lrpc -ltask -laio -lpthread -lm -lrt

SRCS = iosplitter.c client.c
OBJS = $(patsubst %.c,%,$(SRCS))

all: $(OBJS)

server: iosplitter.o 

client: client.o

clean:
	rm -f *.o $(OBJS)

depend: 
	makedepend -s "#BEGIN_DEPEND AUTO GEN BY 'make depend'" -Y -I. -I../include -I.. -m $(SRCS)

#BEGIN_DEPEND AUTO GEN BY 'make depend'

iosplitter.o: ../include/rpc.h ../include/bufpool.h ../libtask/task.h
iosplitter.o: ../include/dll.h ../include/queue.h ../include/cdevtypes.h
iosplitter.o: ../include/cdevcor.h ../include/hash.h ../libtask/taskio.h
iosplitter.o: ../libtask/task.h tst-rpc.h
