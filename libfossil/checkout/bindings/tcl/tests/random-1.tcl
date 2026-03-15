test-family "Random experiments"
fossil --new-instance f .
test savepoint $f db transaction {
  $f db batch {
    savepoint x; create temp table t(a); release x;
  }
}
test commit-hook-vs-begin test-catch-matching \
  {*cannot start a transaction*} {
    $f db transaction {
      $f db batch {
        begin; create temp table t(a); commit;
      }
    }
  }

test commit-hook-vs-begin test-catch-matching \
  {sqlite3 error #1: no such savepoint: fsl_db} {
    $f db transaction {
      test in-transaction assert {1 == [$f db transaction]}
      $f db batch -no-transaction {
        commit;
      }
    }
  }

$f close
