test-family "db open/close" apply {{} {
  test no-db assert {![info exists db]}
  test db-open fossil db open --set db :memory:
  test got-db assert {[info exists db]}
  test is-opened assert {1==[$db open]}
  test batch $db batch {
    create table t(a);
    insert into t(a) values(1),(2),(3);
  }
  test count(t)=3 assert-matches 3 -e {
    $db query --return #0 {select count(*) from t}
  }

  test query-exists assert-matches 1 -e {$db query --return exists {select 3}}
  test query-exists-not assert-matches 0 -e {$db query --return exists {select 3 where 0}}

  test dry-run $db batch -dry-run {
    insert into t(a) values (4)
  }
  test count(t)=3-after-dry-run assert-matches 3 -e {
    $db query --return #0 {select count(*) from t}
  }

  if {0} {
    # If $db close is used from a scope other than the one it was
    # created in, it will not unset the intended var. Same applies to
    # (fossil --new-instance varName).
    set x $db
    proc scoping-issue db {
      test no-db assert {[info exists db]}
      test db-close1 assert {1==[$db close]}
      test no-db assert {![info exists db]}
    }
    test close-from-proc scoping-issue $db
    test no-proc assert {"" eq [info command $x]}
    # db still exists here because it was unset in the proc's scope.
    #test no-db assert {![info exists db]}
    test no-such-proc test-catch-matching \
      {invalid command name "fsl_db*} \
      $db
  } else {
    set x $db
    test no-proc assert {$x eq [info command $x]}
    test db-close1 assert {1==[$db close]}
    test no-proc assert {"" eq [info command $x]}
    test no-db assert {![info exists db]}
  }
}}

test-family "db batch" apply {{} {
  test not-opened test-catch-matching \
    {*database has been closed*} \
    fossil db batch {select 1}

  test open fossil open .

  test batch-sql fossil db batch {
    create temp table tt(a);
    insert into tt(a) values(1),(2);
  }
  test batch-sql-file fossil db batch -file tests/batch1.sql
  test wrong-arg-count test-catch-matching {wrong #*} \
    fossil db batch -file
  test wrong-arg-count test-catch-matching {wrong #*} \
    fossil db batch

  test intentional-error test-catch-matching \
    {sqlite3 error #1: near "this"*} \
    fossil db batch {this is an error}

  test close fossil close
}}

test-family "db query" apply {{} {
  test new-instance-f set x [fossil --new-instance f]
  test name-copy-matches assert-matches $f $x
  test open $f open .
  #unset x

  test mixing-binds test-catch-matching \
    {*--bind-dict, --bind-list, --bind*} \
    $f db query --bind x --bind-list y {sql}

  test invalid-return test-catch-matching \
    {Invalid value for the --return flag*} \
    $f db query {select 1} --return 0

  test bind-list assert-matches \
    {3 integer {} null {} text -non-flag-value text automatic-type text 5 integer 6.1 real} \
    -e {
      $f db query {
        select
        ?1, typeof(?1), ?2, typeof(?2),
        ?3, typeof(?3), ?4, typeof(?4),
        ?5, typeof(?5), ?6, typeof(?6),
        ?7, typeof(?7)
      } --bind-list {
        -integer 3
        -nullable {}
        {}
        -non-flag-value
        -- automatic-type
        5
        -real 6.1
      } --return row1-list
    }

  test old-null-was-empty assert-matches "" [$f db config --null-string MyNull]
  test new-null-is-MyNull assert-matches MyNull [$f db config -null-string]

  test null-is-MyNull assert-matches MyNull -e {
    $f db config -null-string
  }

  test got-custom-null-MyNull assert-matches \
    {MyNull null} \
    -e {
      $f db query {
        select ?1, typeof(?1)
      } --bind-list {
        -null "this is definitely not null"
      } --return row1-list
    }

  test query-null-string-overrides assert-matches \
    {AnotherNull AnotherNull null} \
    -e {
      $f db query --null-string AnotherNull {
        select ?1, null, typeof(null)
      } --bind-list {
        -null "this is definitely not null"
      } --return row1-list
    }

  test null-was-MyNull assert-matches MyNull \
    [$f db config --null-string ""]
  test null-is-empty assert-matches "" \
    [$f db config -null-string]

  test no-such-:Y test-catch-matching \
    {Cannot find column binding named :Y} \
    $f db query { select :X } \
    --bind-map {:Y 1}

  test missing-bind-name test-catch-matching \
    {Missing name for --bind-map*} \
    $f db query { select :X } \
    --bind-map {: 1}

  test malformed-bind-key test-catch-matching \
    {Bind map keys must start with*} \
    $f db query { select :X } \
    --bind-map {x 1}

  test --return-#1 assert-matches -e {
    $f db query {
      select 'one', fsl_dirpart('a/b/c/d', 1)
      -- Only result col 1 of the first row is fetched
    } --return #1
  } {a/b/c/}

  test --return-*1 assert-matches -e {
    $f db query {
      select 'one', fsl_dirpart('a/b/c/d')
      UNION ALL
      select 'two', 'hello'
      -- Collects col 1 of all rows
    } --return *1
  } {a/b/c hello}


  test {--return row1-list} assert-matches -e {
    $f db query {
      select 1,2,3 UNION ALL select 4,5,6
      -- only the first row will be fetched
    } --return row1-list
  } {1 2 3}

  test {--return row1-lists} assert-matches -e {
    $f db query {
      select 1 a, 2 b, 3 c UNION ALL select 4,5,6
      -- only the first row will be fetched
    } --return row1-lists
  } {{a b c} {1 2 3}}


  test {--return rows-lists} assert-matches -e {
    $f db query {
      select 1 a,2 b,3 c UNION ALL select 4,5,6
    } --return rows-lists
  } {{a b c} {1 2 3} {4 5 6}}

  test {--return rows-lists -no-column-names} \
    assert-matches -e {
      $f db query {
        select 1 a,2 b,3 c UNION ALL select 4,5,6
      } --return rows-lists -no-column-names
    } {{1 2 3} {4 5 6}}

  test {--return none} \
    assert-matches -e {
      $f db query {select 1} \
        --return none
    } {}

  test {--return row1-dict} assert-matches -e {
    set x [$f db query {
        select 'one aa' a, 2 b, '3cc' c
    } --return row1-dict]
  } {{*} {a b c} a {one aa} b 2 c 3cc}

  test {--return *} assert-matches {1 2 3} -e {
    $f db query --return * {
      select 1, 2, 3
      union all select 3, 4, 5
    }
  }
  test {--return *} assert-matches {{1 2 3} {3 4 5}} -e {
    $f db query --return ** {
      select 1, 2, 3
      union all select 3, 4, 5
    }
  }

  array set a $x
  unset x
  test confirm-dict-* assert-matches {a b c} $a(*)
  test confirm-dict-a assert-matches {one aa} $a(a)
  test confirm-dict-b assert-matches 2 $a(b)
  test confirm-dict-c assert-matches 3cc $a(c)
  #puts "Result as an array..."; parray a
  array unset a


  test {row1-rict -no-column-names} assert-matches -e {
    set x [$f db query {
        select 'one aa' a, 2 b, '3cc' c
    } --return row1-dict -no-column-names]
  } {a {one aa} b 2 c 3cc}

  test {--return column-names} assert-matches -e {
    set x [$f db query {
        select 1 a, 2 b, 3 c
    } --return column-names]
  } {a b c}


  set i 0
  test {--return *0} assert-matches -e {
    set x [$f db query {
      select 1 a, 2 b
      UNION ALL
      select 3, 4
    } --return *0 ---eval row {
      test row-a assert {$row(a) in {1 3}}
      test row-b assert {$row(b) in {2 4}}
      test row-* assert {$row(*) eq {a b}}
      incr i
    }]
  } {1 3}
  test check-counter assert {2==$i}
  test not-exists-row assert {![info exists row]}

  set i 0
  test --return-#0-with-eval--$ assert-matches -e {
    set x [$f db query {
      select 5 a, 6 b
      UNION ALL
      select 7, 8
    } --return #0 ---eval -$ {
      test exists-a assert {$a in {5 7}}
      test exists-b assert {$b in {6 8}}
      incr i
    }]
  } {5}
  test check-counter assert {2==$i}
  test not-exists-a assert {![info exists a]}
  test not-exists-b assert {![info exists b]}

  set i 0
  test --eval-row--no-column-names assert-matches -e {
    set x [$f db query {
      select 1 a, 2 b
      UNION ALL
      select 3, 4
    } -no-column-names ---eval row {
      test \$row(a) assert {$row(a) in {1 3}}
      test \$row(b) assert {$row(b) in {2 4}}
      test not-exists-\$row(*) assert {![info exists row(*)]}
      incr i
    }]
  } {2}
  test check-counter assert {2==$i}
  test not-exists-row assert {![info exists row]}

  set i 0
  test --eval-$---unset-null assert-matches -e {
    set x [$f db query {
      select 'AAA' a, null b
    } -unset-null --return eval ---eval -$ {
      test exists-a assert {[info exists a]}
      test not-exists-a assert {![info exists b]}
      incr i
    }]
  } {1}; # with no --return, eval will set the result
  test check-counter assert {1==$i}
  test not-exists-a assert {![info exists a]}
  test not-exists-b assert {![info exists b]}

  test not-preexists-1 affirm {![info exists 0]}
  test not-preexists-2 affirm {![info exists 1]}
  test not-preexists-* affirm {![info exists *]}
  #loudly info vars *
  test --return-eval-with-eval--# assert-matches -e {
    set x [$f db query {
      select 5 a, 6 b
      UNION ALL
      select 7, 8
    } ---eval -# {
      test matches-* assert-matches {0 1} ${*}
      test matches-\$0 assert {$0 in {5 7}}
      test matches-\$1 assert {$1 in {6 8}}
      set 1
    }]
  } {8}
  #loudly info vars *
  test not-exists-0 affirm {![info exists 0]}
  test not-exists-1 affirm {![info exists 1]}
  test not-exists-* affirm {![info exists *]}

  $f db query -?
  test query-? assert-matches {query --bind* sqlString} \
    -e {$f db query -?}
  test open-? assert-matches {open -noparents * repoFileOrCheckoutDir} \
    -e {$f open -?}

  test close $f close
}}; # db query

test-family "db transaction" apply {{} {
  test -new-instance set f [fossil -new-instance .]
  test opened assert {0x03 == [$f open]}
  test transaction-level-0 assert {0 == [$f db transaction]}
  test create-table $f db batch {
    create temp table tt(a);
    insert into tt(a) values(1),(2),(3);
    drop table if exists ftcl_batch1;
  }
  test transaction-insert set x [$f db transaction {
    test transaction-level-1 assert {1 == [$f db transaction]}
    test batch-insert $f db batch {insert into tt(a) values(4)}
    expr 3
  }]
  test transaction-result assert {3 == $x}
  test transaction-level-0 assert {0 == [$f db transaction]}
  test tt-count assert {4 == [$f db query --return #0 {
    select count(*) from tt
  }]}
  test transaction-insert-5 $f db transaction -sql {
    insert into tt(a) values(5)
  }
  test check-5 assert {5 == [$f db query --return #0 {
    select max(a) from tt
  }]}
  test rollback-insert-6 $f db transaction -sql -dry-run {
    insert into tt(a) values(6)
  }

  test check-5 assert {5 == [$f db query --return #0 {
    select max(a) from tt
  }]}

  test transaction-propagate-err assert {
    0 != [catch {
      $f db transaction {
        assert {1 == [$f db transaction]}
        $f db batch {insert into tt(a) values(7)}
        error "blah"
      }
    } rc xopt]
  }
  test error-blah assert-matches $rc {blah}
  test check-5 assert {5 == [$f db query --return #0 {
    select max(a) from tt
  }]}
  test transaction-level-0 assert {0 == [$f db transaction]}

  test wrong-arg-count test-catch-matching {wrong #*} \
    $f db transaction -sql
  test wrong-arg-count test-catch-matching {wrong #*} \
    $f db transaction -file
  test wrong-arg-count test-catch-matching {wrong #*} \
    $f db transaction -sql -file

  #loudly $f db query --return #0 {select count(*) from ftcl_batch1}
  test no-such-table test-catch-matching {*no such table*} \
    $f db query --return #0 {select count(*) from ftcl_batch1}

  test transation-from-sql-file \
    $f db transaction -sql -file tests/batch1.sql

  test temp-table-count-3 assert-matches 3 \
    -e {$f db query --return #0 {select count(*) from ftcl_batch1}}

  $f close
}}; # db transaction


test-family {db config} apply {{} {

  set db [fossil db open :memory:]
  #loudly $db config
  test config-no-args test-catch-matching {*expects an arg*} \
    $db config

  test config-busy-timeout assert-matches {} -e {$db config --busy-timeout 1000}

  $db close
}} ; # db config

test-family {db changes} apply {{} {
  set db [fossil db open :memory:]

  test invalid-arg test-catch-matching {*: unexpected* xyz} \
    $db changes xyz

  test changes-0 assert {0==[$db changes]}
  test total-changes-0 assert {0==[$db changes -total]}
  test batch-insert $db batch {
    create table t(a);
    insert into t(a) values(1),(2),(3);
  }
  test changes-3 assert {3==[$db changes]}
  test total-changes-3 assert {3==[$db changes -total]}
  test batch-insert $db batch {
    insert into t(a) values(4),(5);
  }
  test changes-2 assert {2==[$db changes]}
  test total-changes-5 assert {5==[$db changes -total]}

  $db close
}}
