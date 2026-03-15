test is-not-open assert {![fossil open]}
test no-checkout-is-open test-catch-matching {*no opened checkout*} \
  fossil checkout

set f [fossil open -new-instance .]
# A scan is not necessary here because we're not using the
# checkout below - this is only to test that the subcommand
# is dispatching.
test checkout-scan $f checkout scan

set glob {*/tcl/tests/*.tcl}
set li [test ls-glob-1 $f ls --glob $glob]
loudly puts $li
set nSrc 7
test ls-length-$nSrc assert {[llength $li] == $nSrc}

set li2 [test ls-match-1 $f ls --match $glob]
test ls-length-same assert {[llength $li2] == [llength $li]}

test ls-verbose-1 {
  foreach lv [$f ls -verbose --glob $glob] {
    array set a $lv
    assert {[info exists a(name)]}
    assert {[info exists a(uuid)]}
    assert {![info exists a(rename)]}
    assert {[info exists a(perm)]}
    assert {$a(perm) in {x l -}}
    break;
  }
}

test ls-verbose-renames-1 {
  foreach lv [$f ls -verbose -renames --glob $glob] {
    array set a $lv
    assert {[info exists a(name)]}
    assert {[info exists a(rename)]}
    break;
  }
}

test ls-bad-eval test-catch-matching {Missing argument for flag ---eval} \
  $f ls ---eval x

test ls-no-ff assert {![info exists ff]}
set i 0
set x [test ls-eval-1 $f ls --glob $glob ---eval ff {
  incr i
  assert {[info exists ff]}
  assert {[info exists ff(name)]}
  assert {![info exists ff(rename)]}
  assert {[info exists ff(uuid)]}
  assert {[info exists ff(perm)]}
  #assert {![info exists ff(fossil)]}
  assert-matches fsl_cx#* $f
}]
test ls-no-result-val assert {"" eq $x}
test ls-still-no-ff assert {![info exists ff]}
test ls-eval-count-${nSrc} assert {$i == $nSrc}

# Check info for a specific file/version...
set ver {1ec5e365a097dd643a1}
set fn bindings/tcl/teaish.test.tcl.in
set tm {2025-06-03 12:32:53}
set sz 28661
test ls-size-time {
  $f ls --version $ver --glob $fn -time -size ---eval ff {
    test file-name-match assert-matches $fn $ff(name)
    test file-size-match assert-matches $sz $ff(size)
    test file-time-match assert-matches $tm $ff(time)
    test file-not-renamed assert {![info exists ff(rename)]}
  }
}
TODO test ls-size-time {
  # FIXME: find a version where we had a rename and add a test for
  # it.
  $f ls --version $ver --glob $fn -time -size ---eval ff {
    test file-name-match assert-matches $fn $ff(name)
    test file-size-match assert-matches $sz $ff(size)
    test file-time-match assert-matches $tm $ff(time)
    #test file-renamed assert-matches TODO $ff(rename)
  }
}

$f close
