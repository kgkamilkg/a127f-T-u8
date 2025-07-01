#!/bin/bash
PATCHDIR=$(dirname "$0")/scripts
patch -N -p1 < "$PATCHDIR/fix_yylloc.patch"
