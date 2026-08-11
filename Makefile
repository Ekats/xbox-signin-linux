# make predefines CC as "cc", so `?=` would never take effect. Only override
# it when it is still make's own default -- an explicit CC= on the command
# line or in the environment still wins.
ifeq ($(origin CC),default)
CC = x86_64-w64-mingw32-gcc
endif

CFLAGS ?= -O2 -Wall -mwindows
LDLIBS ?= -luser32 -lkernel32
OUT     = WebClient.exe

$(OUT): src/webclient.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

.PHONY: install uninstall clean
install: $(OUT)
	./install.sh

uninstall:
	./uninstall.sh

clean:
	rm -f $(OUT)
