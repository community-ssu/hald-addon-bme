all: hald-addon-bme

install: all
	install -d "$(DESTDIR)/usr/lib/hal/hald-addon-bme"
	install -m 755 hald-addon-bme "$(DESTDIR)/usr/lib/hal/"

clean:
	$(RM) hald-addon-bme

hald-addon-bme: hald-addon-bme.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(shell pkg-config --libs --cflags glib-2.0 hal dbus-glib-1) -lm -W -Wall -O2
