#!/bin/sh

set -x
libtoolize --copy --automake

aclocal-1.8
autoconf
autoheader
automake-1.8 --add-missing --copy --foreign
./configure --enable-maintainer-mode
