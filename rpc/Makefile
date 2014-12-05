CFLAGS += -Wall -Werror -ggdb
LDFLAGS += -ggdb

EXE_SRCS = tst_threadpool.c tst-network.c tst-rpc-client.c tst-rpc-server.c
EXE_OBJS = $(patsubst %.c,%.o,$(EXE_SRCS))
EXES = $(patsubst %.c,%,$(EXE_SRCS))

SRCS = bufpool.c hash.c queue.c threadpool.c network.c rpc.c
OBJS = $(patsubst %.c,%.o,$(SRCS))

LDLIBS = -lpthread

.PHONY: all
all: $(OBJS) $(EXE_OBJS) $(EXES)

tst_threadpool: $(SRCS)
tst-network: $(SRCS)
tst-rpc-client: $(SRCS)
tst-rpc-server: $(SRCS)

clean:
	rm -rf $(OBJS) $(EXES) $(EXE_OBJS)
