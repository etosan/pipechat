
################################
# requires: GNU make aka gmake #
################################

PREFIX := /usr/local

PLATFORM := $(shell exec uname -s)

ifeq ($(PLATFORM),FreeBSD)
  CC := clang
  LDFLAGS := -I/usr/local/include -L/usr/local/lib
else
  CC := gcc
  LDFLAGS :=
endif

LDFLAGS := $(LDFLAGS) -lreadline

all: pipechat

pipechat: pipechat.c
	@echo "Platform: $(PLATFORM)"
	$(CC) -o pipechat pipechat.c $(CFLAGS) $(LDFLAGS)

clean:
	rm -f pipechat

install: pipechat
	/usr/bin/install -t $(PREFIX)/bin pipechat
	/usr/bin/install -t $(PREFIX)/share/man/man1 pipechat.1
