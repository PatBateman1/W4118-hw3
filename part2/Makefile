CC = gcc
CFLAGS = -g -Wall -Werror
LDFLAGS = -g -lpthread

TARGETS = multi-server
OBJS = multi-server.o

$(TARGETS):
$(OBJS):

PHONY += clean
clean:
	rm -rf $(TARGETS) a.out *.o

.PHONY: $(PHONY)
