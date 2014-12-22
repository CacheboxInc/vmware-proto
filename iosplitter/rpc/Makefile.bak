# cache device manager skeleton code
# using libtask coroutines

CFLAGS += -Wall -g -D CDEV_LIBTASK -I../include -I../
SRCS = rpc.c ../common/queue.c ../common/bufpool.c ../common/hash.c
OBJS = $(patsubst %.c,%.o,$(SRCS))
LIB = librpc.a

all : $(LIB)

$(LIB): $(OBJS)
	ar rvc $(LIB) $(OBJS)

clean:
	rm -f *.o $(LIB)

depend : 
	makedepend -s "#BEGIN_DEPEND AUTO GEN BY 'make depend'" -Y -I. -I.. -I../include -m $(SRCS)

#BEGIN_DEPEND AUTO GEN BY 'make depend'

rpc.o: ../include/rpc.h ../include/bufpool.h ../libtask/task.h
rpc.o: ../include/dll.h ../include/queue.h ../include/cdevtypes.h
rpc.o: ../include/cdevcor.h ../include/hash.h ../libtask/taskio.h
rpc.o: ../libtask/task.h
../common/queue.o: ../include/queue.h ../include/dll.h
../common/bufpool.o: ../include/bufpool.h ../libtask/task.h ../include/dll.h
../common/bufpool.o: ../include/queue.h ../include/cdevtypes.h
../common/bufpool.o: ../include/cdevcor.h
../common/hash.o: ../include/hash.h ../include/dll.h
