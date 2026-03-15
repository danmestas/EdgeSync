#!/usr/bin/env bash
#
# Copies source files from the upstream fnc source tree and creates
# ./fnc-version.tcl with version info for use by ../../auto.def and
# friends.

if [ x = "x$1" ]; then
  echo "Usage: $0 /path/to/upstream/fnc" 1>&2
  exit 1
fi

set -e
d=$1

cp $d/src/*.c $d/include/*.h .
{
  echo "define FNC_HASH {"$(cat $d/manifest.uuid)"} ;"
  echo "define FNC_DATE {"$(awk -e '/^D /{print $2}' $d/manifest | tr T ' ')"} ;"
  echo "define FNC_VERSION {"$(awk -e '/^VERSION */{print $3}' $d/fnc.bld.mk)"} ;"
} > fnc-version.tcl
