#!/bin/bash

# fpp-smpte uninstall script
echo "Running fpp-smpte uninstall Script"

BASEDIR=$(dirname $0)
cd $BASEDIR
cd ..
make clean

