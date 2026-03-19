APP = oled-gpio-app

CC ?= gcc
CFLAGS += -Wall -O2
LDFLAGS += -lm

SRC = oled-gpio-app.c
OBJ = $(SRC:.c=.o)

all: $(APP)

$(APP): $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(APP) $(OBJ)

install: $(APP)
	install -d $(DESTDIR)/usr/bin
	install -m 0755 $(APP) $(DESTDIR)/usr/bin/

.PHONY: all clean install
