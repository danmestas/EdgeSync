test-family "Scalar UDFs" apply {{} {
  #fossil option --verbosity 3
  set db [fossil db open :memory:]
  #fossil option --verbosity 0
  test function-? assert-matches {function -*functionName*} -e {
    loudly $db function -?
  }

  test function-missing-flags test-catch-matching \
    {*--scalar *} {
      $db function bad
    }

  test function-bad--return test-catch-matching \
    {Invalid value for the --return flag*} {
      $db function bad --return wrong --scalar {{} {}}
    }

  test function-bad--scalar-1 test-catch-matching \
    {*xFunc *at least 1*} {
      $db function bad --scalar {{} {}}
    }

  test function-bad--scalar-2 test-catch-matching \
    {*two-entry list*} {
    $db function bad --scalar {a}
  }

  test function-foo $db function foo \
    -bad-numbers-as-0 \
    --return real \
    --scalar {{db args} {
      return $args
    }}

  test foo-1 assert-matches {1 2 3} -e {
    $db query {select foo(1,2,3)}
  }

  test foo-2 assert-matches {1 hi {}} -e {
    $db query {select foo(1,'hi',null)}
  }

  set ::ii 0
  set dbName ""
  test function-bar $db function bar --scalar [subst -nocommands {{ddb} {
    # func cannot access vars in the caller's scope
    test db-for-udf assert-matches $db \$ddb
    incr ::ii
  }}]

  test bar-1 assert-matches {1 2 3} -e {
    $db query --return row1-list {select bar(), bar(), bar()}
  }
  test ::i==3 assert-matches 3 -e {set ::ii}
  unset dbName

  test bar-2 test-catch-matching {sqlite3 error #1: bar.xFunc() argument count*} {
    $db query {select bar(1)}
  }

  test bar-3 $db query {select bar(), bar(), bar()}

  test ::i==6 assert {$::ii == 6}

  set ::i 0
  test function-baz $db function baz --scalar {{db a b {c 3} {d 4} args} {
    incr ::ii
    expr {$a + $b + $c + $d + [llength $args]}
  }}

  test baz-1 test-catch-matching {sqlite3 error #1: baz.xFunc() argument count*} {
    $db query {select baz(1)}
  }

  test adds-to-10 assert-matches 10 -e {$db query {select baz(1,2)}}
  test adds-to-14 assert-matches 14 -e {$db query {select baz(1,2,4,5,11,12)}}
  #puts "exiting for testing"; exit 1

  test function-cannull $db function cannull -return-{}-as-null \
    --scalar {{db arg} {return $arg}}

  test cannull-1 assert-matches -e {
    $db query --return row1-list -no-column-names {
      select typeof(cannull(1)),
      typeof(cannull('')),
      typeof(cannull('hi'))
    }
  } {integer null text}

  if {0} {
    # This (A) isn't working (UDF-local var is not being set, despite
    # being able to fetch it from C) and (B) potentially has horrible
    # repercussions when a UDF recursively triggers.
    test no-darg assert {![info exists darg]}
    test function--meta-args $db function meta --meta-args darg --scalar {{} {
      loudly puts "$darg"
      loudly info vars
      test got-darg assert {[info exists darg]}
      loudly puts "darg=$darg"
      return 1
      #loudly parray darg
    }}
    test function-meta-query $db query {
      select meta()
    }
    test still-no-darg assert {![info exists meta]}
    puts "EXITING PREMATURELY BY CHOICE"; exit
  }; # --meta-args

  $db close
}}; # scalar UDFs

test-family "Aggregate UDFs" apply {{} {
  set db [fossil db open :memory:]

  test ---aggregate-with--scalar test-catch-matching \
    {None of* may be used together} {
      $db function foo --scalar b ---aggregate x y
    }

  test missing-callback test-catch-matching \
    {Use --* callback*} {
      $db function foo
    }

  array set ::aggx {}
  set ::totalAggCx 0
  set xStep {{db aggCx args} {
    #puts "xStep! $db $aggCx ($args)"
    #test xStep-aggCx-is-set assert {"" ne $aggCx}
    if {![info exists ::aggx($aggCx)]} {
      incr ::totalAggCx
    }
    incr ::aggx($aggCx)
  }}

  set xFinal {{db aggCx} {
    #puts "xFinal! db=$db aggCx=$aggCx"
    if {[info exists ::aggx($aggCx)]} {
      #puts "xFinal $aggCx ::aggx"; parray ::aggx
      set x $::aggx($aggCx)
      unset ::aggx($aggCx)
      return $x
    }
    return -99
  }}

  test function-agg $db function foo ---aggregate $xStep $xFinal
  test agg-select-foo-x2 $db batch {
    create table t(a);
    insert into t(a) values(1),(2),(3);
    select foo(a),foo(a*2) from t order by a;
  }
  test aggregate-cx-count-2 assert {2 == $::totalAggCx}

  test agg-xFinal-value assert-matches -e {
    $db query --return #0 {
      select foo(a) as f from t order by a DESC
    }
  } 3

  test aggregate-cx-count-3 assert {3 == $::totalAggCx}

  test agg-xFinal-default-value assert-matches -e {
    $db query --return #0 {
      select foo(1) where 0
    }
  } -99

  test aggregate-cx-count-still-3 assert {3 == $::totalAggCx}

  #puts "final ::aggx"; parray ::aggx
  array unset ::aggx
  unset ::totalAggCx
  $db close
}}; # Aggregate UDFs

test-family "Window UDFs" apply {{} {
  set db [fossil db open :memory:]

  test -----window-with--scalar test-catch-matching \
    {None of* may be used together} {
      $db function bad --scalar a -----window x y z q
    }

  array set ::aggx {}
  set ::totalAggCx 0
  set xStep {{db aggCx arg} {
    if {![info exists ::aggx($aggCx)]} {
      incr ::totalAggCx
      set ::aggx($aggCx) 0
    }
    #puts "xStep($arg) $db $aggCx = pre-add: $::aggx($aggCx)"
    incr ::aggx($aggCx) $arg
    #puts "xStep($arg) $db $aggCx = $::aggx($aggCx)"
  }}

  set xFinal {{db aggCx} {
    if {[info exists ::aggx($aggCx)]} {
      set x $::aggx($aggCx)
      #puts "xFinal/xValue! db=$db aggCx=$aggCx = $x"
      #puts "xFinal $aggCx ::aggx"; parray ::aggx
      unset ::aggx($aggCx)
      return $x
    }
    return 0
  }}

  set xValue {{db aggCx} {
    if {[info exists ::aggx($aggCx)]} {
      set x $::aggx($aggCx)
      #puts "xValue! db=$db aggCx=$aggCx = $x"
      #puts "xValue $aggCx ::aggx"; parray ::aggx
      return $::aggx($aggCx)
    }
    return 0
  }}

  set xInverse {{db aggCx arg} {
    #test xInverse-agg-exists assert {[info exists ::agg($aggCx)]}
    if {![info exists ::aggx($aggCx)]} {
      # The SQLite docs say this is impossible, so i've maybe got a
      # bug somewhere: "[xInverse] is only ever called after xStep()
      # (so the aggregate context has already been allocated)"
      incr ::totalAggCx
      set ::aggx($aggCx) 0
    }
    incr ::aggx($aggCx) -$arg
  }}

  # Example taken from https://sqlite.org/windowfunctions.html#udfwinfunc
  test window-setup-db $db batch {
    CREATE TABLE t3(x, y);
    INSERT INTO t3 VALUES('a', 4),
                         ('b', 5),
                         ('c', 3),
                         ('d', 8),
                         ('e', 1);
  }

  test window-func-foo $db function foo \
    -----window $xStep $xFinal $xValue $xInverse

  test window-foo-results1 {
    assert-matches -e {
      $db query --return rows-lists \
        -no-column-names {
          SELECT x, foo(y) OVER (
            ORDER BY x ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING
          ) AS sum_y
          FROM t3 ORDER BY x;
        }
    } {{a 9} {b 12} {c 16} {d 12} {e 9}}
  }

  $db close
}}; # Window UDFs
