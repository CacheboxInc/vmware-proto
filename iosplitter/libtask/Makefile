# modified libtask
# uses epoll/libaio interface
# changes:
#  fd.o removed from OFILES
#  taskio.o added to OFILES
#  add taskio.h 

LIB=libtask.a
TCPLIBS=

ASM=asm.o
OFILES=\
	$(ASM)\
	channel.o\
	context.o\
	taskio.o\
	net.o\
	print.o\
	qlock.o\
	rendez.o\
	task.o\
	timer.o

all: $(LIB) 

testtask : testtask.c
	$(CC) -Wall -I. -ggdb -o testtask -I../include testtask.c $(LIB) -lpthread -laio

examples : primes tcpproxy testdelay

$(OFILES): taskimpl.h task.h 386-ucontext.h power-ucontext.h taskio.h

AS=gcc -c
CC=gcc
CFLAGS +=-Wall -c -I. -ggdb -DVALGRIND

%.o: %.S
	$(AS) $*.S $(CFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) $*.c

$(LIB): $(OFILES)
	ar rvc $(LIB) $(OFILES)

primes: primes.o $(LIB)
	$(CC) -o primes primes.o $(LIB)

tcpproxy: tcpproxy.o $(LIB)
	$(CC) -o tcpproxy tcpproxy.o $(LIB) $(TCPLIBS)

httpload: httpload.o $(LIB)
	$(CC) -o httpload httpload.o $(LIB)

testdelay: testdelay.o $(LIB)
	$(CC) -o testdelay testdelay.o $(LIB)

testdelay1: testdelay1.o $(LIB)
	$(CC) -o testdelay1 testdelay1.o $(LIB)

clean:
	rm -f *.o primes tcpproxy testdelay testdelay1 httpload $(LIB)

install: $(LIB)
	cp $(LIB) /usr/local/lib
	cp task.h /usr/local/include
	cp taskio.h /usr/local/include

uninstall :
	rm /usr/local/lib/$(LIB)
	rm /usr/local/include/task.h
	rm /usr/local/include/taskio.h
