test new-instance-f fossil --new-instance f
test no-repo-file assert-matches {} [$f info -repo-db]
test no-checkout-file assert-matches {} [$f info -checkout-dir]
test no-multiple-flags test-catch-matching \
  {Only one info flag*} \
  {$f info -repo-db -checkout-db}
test open-$f-. $f open .
test open-0x3 assert {0x3 == [$f open]}
test repo-exists assert [file exists [$f info -repo-db]]
test ckout-db-exists assert [file exists [$f info -checkout-db]]
test ckout-dir-exists assert [file isdirectory [$f info -checkout-dir]]
test info-? assert-matches {*-repo-db -checkout-db*} \
  [$f info -?]
test close $f close
