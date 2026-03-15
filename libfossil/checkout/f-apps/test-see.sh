#!/usr/bin/bash
#
# A very basic test for SEE-enabled builds.

function die() {
  local rc=$1
  shift
  echo "FATAL:" $@ 1>&2
  exit $rc
}

if [[ 0 = $(ls -1 ../extsrc/*-see*.c | wc -l) ]]; then
  die 127 "Cannot find *-see*.c in ../extsrc"
fi
pw=foo
r=see.db
sargs="-R $r --see-key $pw"
rm -f $r
./f-new -m 'encrypted db' $r --see-key $pw || die $? "f-new failed"
./f-ls $sargs || die $? "f-ls failed"
ln0=$(./f-acat trunk $sargs | wc -l)
test 8 = $ln0 || die 1 "Unexpected manifest line count"
./f-ciwoco $sargs -m "a checkin" f-ciwoco.c || die $? "f-ciwoco failed."
ln1=$(./f-acat trunk $sargs | wc -l)
test 7 = $ln1 || die 1 "Unexpected manifest line count after checkin"
./f-acat trunk $sargs | grep f-ciwoco.c || die $? "Missing expected checked-in file"
rm -f $r
echo "Done"
