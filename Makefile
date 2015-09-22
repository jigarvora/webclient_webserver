CFLAGS := -Wall --std=gnu99 -g

ARCH := $(shell uname)
ifneq ($(ARCH),Darwin)
  LDFLAGS += -lpthread -pthread
endif

default: webserver webclient
webserver: webserver.o log.o
webclient: webclient.o log.o

.PHONY: clean

clean:
	rm -fr *.o webserver webclient
