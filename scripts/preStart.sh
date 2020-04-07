#!/bin/sh

echo "Running fpp-smpte PreStart Script"

BASEDIR=$(dirname $0)
cd $BASEDIR
cd ..
make
