#!/bin/sh
#
# Copyright (C) 2010-2017 Red Hat, Inc.  All rights reserved.
#
# Author: Fabio M. Di Nitto <fabbione@kronosnet.org>
#
# This software licensed under GPL-2.0+, LGPL-2.0+
#

# Run this to generate all the initial makefiles, etc.
mkdir -p m4
autoreconf -i -v && echo Now run ./configure and make