test-family sanity-checks {
  test fossil-cmd assert {"fossil" eq [info command fossil]}

  test fossil-? assert-matches {fossil --new-instance*-?} -e {
    fossil -?
  }

  test get-version assert {0 == [fossil option -get -verbose]}
  test verbosity-3 fossil option --verbosity 3
  test verbosity-set assert {3 == [fossil option -get -verbose]}
  test verbosity-0 fossil option --verbosity 0
  test verbosity-get assert {0 == [fossil option -get -verbose]}
  test -verbose fossil option -verbose
  test {-get -verbose} assert {1 == [fossil option -get -verbose]}
  test invalid-verbosity test-catch-matching {*integer*} fossil option --verbosity nope
  test-catch-matching \
    {-get requires other flags} \
    fossil option -get
  test option-get-verbose assert {1 == [fossil option -get -verbose]}
  test verbosity-off-again fossil option --verbosity 0

  test exec-no-repo test-catch-matching \
    {*database has been closed*} \
    fossil db query {select 1}

  test not-opened assert {0 == [fossil db open]}

  test db.info-name assert-matches {} -e {
    fossil db info -name
  }
  test db.info-file assert-matches {} -e {
    fossil db info -file
  }

  test open-. fossil open .
  test open-again test-catch-matching \
    {This fossil instance is already open*} \
    fossil open .
  test is-open assert {0x3 == [fossil open]}
  test db.info-name assert-matches {ckout} -e {
    fossil db info -name
  }
  test db.info-file assert-matches {*.*} -e {
    fossil db info -file
  }

  test close fossil close
  test close-again fossil close; # harmless no-op

  test ckout-not-found test-catch-matching \
    {Could not find checkout*} \
    fossil open -noparents .

  test open-new-instance-f. fossil open --new-instance f .
  test exists-f assert {[info exists f]}
  test expected-\$f-name assert-matches fsl_cx#* $f
  test not-exists-$f assert {![info exists $f]}
  test not-exists-cmd-f assert {"" eq [info command f]}
  test exists-cmd-$f assert {$f eq [info command $f]}
  test not-exists-proc-f assert {"" eq [info proc f]}
  test not-exists-proc-$f assert {"" eq [info proc $f]}
}

test-family {fossil resolve} apply {{f} {
  test cannot-resolve test-catch-matching \
    {Could not resolve*} \
    $f resolve no-such-thing

  test -noerr assert {"" eq [$f resolve -noerr no-such-thing]}
  test -noerr assert {0 == [$f resolve -noerr -rid no-such-thing]}
  test -rid assert {0 < [$f resolve -rid {57161bda6f262e1cbfda62c73e58b2cb291bc42c048b8b50faa60e9c2d34da90}]}

  proc passed-a-fsl {ff} {
    test passed-a-fossil-$ff assert {
      1 == [$ff resolve -rid 99237c3636730f20ed07b227c5092c087aea8b0c]
    }
    test passed-a-fossil-$ff assert {
      "" ne [$ff resolve -rid rid:1]
    }
  }

  passed-a-fsl $f
  rename passed-a-fsl ""
}} $f

test-family fossil-close-and-reopen {
  #puts "Closing... f ($f)"
  test close-keep $f close -keep
  #puts "Closed but kept"
  test info-exists-f assert {[info exists f]}
  test re-open $f open .
  #puts "Re-opened"

  set fname $f
  puts "Closing and deleting (should trigger finalizer)..."
  test close $f close
  test not-exists-f assert {![info exists f]}
  test not-exists-cmd-$fname assert {"" eq [info command $fname]}
  puts "Closed and deleted. Again with open -new-instance..."
  unset fname

  test-family new-instance
  test separate-instance set f [fossil open -new-instance]
  test exists-cmd-$f assert {$f eq [info command $f]}
  test name-matches assert-matches fsl_cx#* $f
  test unknown-cmd-foo test-catch-matching \
    {Unknown subcommand "foo". Try:* open *} \
    $f foo

  test open $f open .
  test rename-$f-{} rename $f ""
  unset f
}

test-family "fossil option subcommand" apply {{} {
  test test1 assert {{a b} eq [fossil option --test a -get --test b]}
  test test2 assert {{{c d} {e f g}} eq [fossil option ---test {c d} {e f g} -get]}
}}
