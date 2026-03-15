#!/usr/bin/env bash
set -x -e
rm -f x.f; ./f-new x.f -m 'sanity test'; rm -f x.f
./f-sanity
./test-ciwoco
./f-parseparty -q

echo "If you made it this far, the basics still work."
