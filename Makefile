all: hald-addon-bme

install: all
	install -d "$(DESTDIR)/usr/lib/hal/"
	install -d "$(DESTDIR)/usr/share/hal/fdi/policy/10osvendor"
	install -d "$(DESTDIR)/etc/dbus-1/system.d"
	install -d "$(DESTDIR)/usr/include/bme-dbus-proxy"
	install -m 755 hald-addon-bme "$(DESTDIR)/usr/lib/hal/"
	install -m 644 10-bme.fdi "$(DESTDIR)/usr/share/hal/fdi/policy/10osvendor/"
	install -m 644 hald-addon-bme.conf "$(DESTDIR)/etc/dbus-1/system.d/"
	install -m 644 dbus-names.h "$(DESTDIR)/usr/include/bme-dbus-proxy/"

uninstall:
	$(RM) "$(DESTDIR)/usr/lib/hal/hald-addon-bme"
	$(RM) "$(DESTDIR)/usr/share/hal/fdi/policy/10osvendor/10-bme.fdi"
	$(RM) "$(DESTDIR)/etc/dbus-1/system.d/hald-addon-bme.conf"
	$(RM) "$(DESTDIR)/usr/include/bme-dbus-proxy/dbus-names.h"

clean:
	$(RM) hald-addon-bme

hald-addon-bme: hald-addon-bme.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(shell pkg-config --libs --cflags glib-2.0 hal dbus-glib-1 dsme) -lm -W -Wall -O2
