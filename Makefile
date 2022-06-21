SHELL	= /bin/bash -O extglob -c
CC	= g++
CXX	= g++
BUILD_COMMIT = "$(shell git log -n 1 --pretty=oneline | awk '{print $$1}')"
CFLAGS	= -Wall -Wextra -pedantic -pedantic-errors -g -O2 -DLINUX -DREDAX_BUILD_COMMIT='$(BUILD_COMMIT)' -std=c++17 -pthread $(shell pkg-config --cflags libmongocxx)
CPPFLAGS := $(CFLAGS)
IS_READER0 := false
ifeq "$(shell hostname)" "reader0"
	IS_READER0 = true
endif
LDFLAGS = -lCAENVME -lstdc++fs -llz4 -lblosc $(shell pkg-config --libs libmongocxx) $(shell pkg-config --libs libbsoncxx)
#LDFLAGS_CC = ${LDFLAGS} -lexpect -ltcl8.6

SOURCES_SLAVE = CControl_Handler.cc DAQController.cc f1724.cc main.cc MongoLog.cc \
				Options.cc StraxFormatter.cc V1495.cc V1724.cc V1724_MV.cc \
 				V1730.cc V1725.cc V2718.cc f2718.cc V1495_tpc.cc
OBJECTS_SLAVE = $(SOURCES_SLAVE:%.cc=%.o)
DEPS_SLAVE = $(OBJECTS_SLAVE:%.o=%.d)
EXEC_SLAVE = redax

ifeq "$(IS_READER0)" "true"
 	SOURCES_SLAVE += DDC10.cc
	CFLAGS += -DHASDDC10
	LDFLAGS += -lexpect -ltcl8.6
endif

all: $(EXEC_SLAVE)

$(EXEC_SLAVE) : $(OBJECTS_SLAVE)
	$(CC) $(OBJECTS_SLAVE) $(CFLAGS) $(LDFLAGS) -o $(EXEC_SLAVE)

%.d : %.cc
	@set -e; rm -f $@; \
	$(CC) -MM $(CFLAGS) $< > $@.$$$$; \
	sed 's,\($*\)\.o[ :]*,\1.o $@ : ,g' < $@.$$$$ > $@; \
 	rm -f $@.$$$$ 

%.o : %.cc %.d
	$(CC) $(CFLAGS) -o $@ -c $<

.PHONY: clean

clean:
	rm -f *.o *.d 
	rm -f $(EXEC_SLAVE )

include $(DEPS_SLAVE)

