#!/bin/bash
# A quick hack to (fossil patch pull --force) from another dir
# whenever $infile (in that dir) is modified.  Requires inotifywait
# (from the inotify-tools package). Intended to be run from the top of
# the source tree.

updir=${1-/home/ram/f} # working dir containing a checkout of this repo
if ! [ -d $updir ]; then
  echo "Upstream dir not found: $updir"
  exit 1
fi
rc=0
infile=$updir/.made # gets updated every time make is run
while true; do
  if [ 0 = $rc ]; then
    fossil patch pull --force $updir
  fi
  echo
  date
  echo "Monitoring for changes to build-relevant content files."
  echo "Tap Ctrl-C to quit."
  while true; do
    inotifywait \
      -e delete \
      -e close_write \
      --timefmt '%Y-%m-%d %H:%M:%S' \
      --format '******************** %T %e %w' \
      $infile
    rc=$?
    # It exits with rc=1 when we remove a watched file from the tree,
    # thus we don't (while inotifywait...; do... done). Related: it's
    # not outputing the --format for DELETE events.
    #
    # Aaaaarrrrggggg. It also returns rc=1 for invalid arguments.
    #
    # Aaaarrrrgggg: it just randomly stops reporting events some of
    # the time, for no apparent reason.
    if [[ $rc -gt 2 ]]; then
      echo "$(date) Unexpected result code: $rc" 1>&2
      break
    fi
    #echo "rc=$rc"
    if [[ $rc -eq 1 ]]; then
      echo "$(date) Trigger file not found. Waiting a bit for it."
      sleep 10
      continue
    fi
    break
  done
#    case $rc in
#         0|1|2) continue ;;
#         *) echo "inotifywait returned unexpected code $rc";
#            exit $rc
#    esac
done

exit $rc
