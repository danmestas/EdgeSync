# Tests for the (fossil db query -prepare) (stmt command)
# pieces.

test-subfamily {Closing db} {
  test db-open-1 fossil db open --set db :memory:
  #$db option --verbosity 3
  #loudly puts "db=$db"
  test not-exists-q assert {![info exists q]}
  test prepare-1 $db query --prepare q {select 1}
  #loudly puts "q=$q"
  test exists-q assert {[info exists q]}

  test step-is-true assert {1 == [$q step]}
  test step-is-false assert {0 == [$q step]}
  test reset-stmt $q reset
  test step-is-true assert {1 == [$q step]}
  test step-is-false assert {0 == [$q step]}
  test finalize-q $q finalize
  test not-exists-q-2 assert {![info exists q]}

  $db close
  test not-exists-db assert {![info exists db]}

  test db-open-2 fossil db open --set db :memory:
  test prepare-2 $db query --prepare q {select 2}
  test step-is-true assert {1 == [$q step]}
  $db close
  # Will close the dbs and unset db but q still holds a ref to the db
  # context. Its prepared statement, however, will be invalidated.
  test query-invalidated test-catch-matching \
    {*invalidated because its db was closed} \
    $q step
  test query-finalize $q finalize
}

test-subfamily {Closing fossil instance} {
  fossil open --new-instance f .
  #$f option --verbosity 3
  #loudly puts "f=$f"
  test not-exists-q assert {![info exists q]}
  test prepare-1 $f db query --prepare q {select 1}
  #loudly puts "q=$q"
  test exists-q assert {[info exists q]}
  test step-is-true assert {1 == [$q step]}
  test closing-fossil $f close
  # Will close the dbs and unset f but q still holds a ref to $f.  Not
  # that that ref does much good because the dbs have been
  # closed. That would be bad, bad, bad, so its prepared statement has
  # been invalidated.
  test not-exists-db assert {![info exists f]}

  test stmt-invalid-because-fossil-closed test-catch-matching \
    {*invalidated because its db was closed} \
    $q step

  test stmt-finalize $q finalize
  test not-exists-q assert {![info exists q]}

}

test-subfamily {stmt get} {
  fossil db open --set db :memory:
  test prepare-1 $db query {
    select 3 a, 'hi' b, NULL c
  } --prepare q --null-string MYNULL

  test column-names-match assert-matches {a b c} -e {
    $q get -column-names
  }

  test step-is-true assert {1 == [$q step]}
  test column-count-is-3 assert-matches 3 -e {$q get -count}
  test column-0-is-3 assert-matches 3 -e {$q get 0}
  test column-1-is-hi assert-matches "hi" -e {$q get 1}
  test column-2-is-null assert-matches "MYNULL" -e {$q get 2}

  test column-2-is-different-null assert-matches "AnotherNull" -e {
    $q get --null-string AnotherNull 2
  }

  test column-3-out-of-range test-catch-matching \
    {Column index 3 is out of range} $q get 3

  test get-list assert-matches {3 hi MYNULL} -e {
    $q get -list
  }

  test reset-q $q reset
  test step-again assert {[$q step]}
  test get-list-after-reset assert-matches {3 hi MYNULL} -e {
    $q get -list
  }

  set dict [$q get -dict]
  #loudly puts "dict=$dict"
  test dict-matches assert-matches {a 3 b hi c MYNULL} $dict
  array set row $dict
  test row-has-a assert-matches "3" $row(a)
  test dict-has-a assert-matches "3" -e {dict get $dict a}

  test row-has-b assert-matches hi $row(b)
  test dict-has-b assert-matches hi -e {dict get $dict b}

  test row-has-c assert-matches MYNULL $row(c)
  test dict-has-c assert-matches MYNULL -e {dict get $dict c}

  test row-has-no-* assert {![info exists row(*)]}

  $q finalize
  $db close
}

test-subfamily {stmt bind} {
  fossil db open --set db :memory:
  test prepare-1 $db query {
    select :A a, :B b, :C c
  } --prepare q

  test column-names assert-matches {a b c} -e {
    $q get -column-names
  }

  test param-clear assert {"" eq [$q bind -clear]}
  test param-count-3 assert {3 == [$q bind -count]}

  test no-type-flags $q bind -no-type-flags --map {
    :A -null
    :B :BB
    :C -text
  }
  test step-ok assert {1 == [$q step]}
  test bound-map-result assert-matches {-null :BB -text} -e {
    $q get -list
  }

  test bind-without-reset-throws test-catch-matching \
    {sqlite3 error #21: bad parameter or other API misuse} {
      $q bind --map {:A 1}
    }

  test bind-map-missing-value test-catch-matching \
    {Missing binding-map value for key :C} {
      $q bind -reset --map {:A 1 :B 2 :C}
    }

  $q finalize
  test prepare-1 $db query {
    select ?1 a, ?2 b, ?3 c
  } --prepare q

  test too-few-binds-is-okay $q bind -no-type-flags --list {1 2}
  test bind-list-too-beaucoup test-catch-matching \
    {Bind column #4 is out of bounds*} {
      $q bind --list {1 2 3 4}
    }


  $q finalize
  $db close
}
