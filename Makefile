# Macros
#CC = gcc
CCFLAGS = -g
CCFLAGS += -DNDEBUG
CCFLAGS += -O3
#CCFLAGS += -O1
CCFLAGS += -DNO_DPDK
#CCFLAGS += -debug inline-debug-info
CCFLAGS += -I$(PWD)/../../../fastpass-public/src/graph-algo
#CCFLAGS += -DAUTO_RE_REQUEST_BACKLOG
CCFLAGS += -DEMULATION_ALGO
LDFLAGS = -lm
#LDFLAGS = -debug inline-debug-info

# Pattern rule
%.o: %.c
	$(CC) $(CCFLAGS) -c $<

# Dependency rules for non-file targets
all: emulation
clean:
	rm -f emulation *.o *~

# Dependency rules for file targets
emulation: emulation_test.o emulation.o endpoint.o router.o packet.o port.o drop_tail_endpoint.o drop_tail_router.o
	$(CC) $< emulation.o endpoint.o router.o drop_tail_endpoint.o packet.o port.o drop_tail_router.o -o $@ $(LDFLAGS)
