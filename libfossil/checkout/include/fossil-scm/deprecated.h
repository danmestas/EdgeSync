/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */ 
/* vim: set ts=2 et sw=2 tw=80: */
#if !defined(FSL_OMIT_DEPRECATED)
#if !defined(ORG_FOSSIL_SCM_LIBFOSSIL_DEPRECATED_H_INCLUDED)
#define ORG_FOSSIL_SCM_LIBFOSSIL_DEPRECATED_H_INCLUDED
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/

/** @file deprecated.h

  This file holds APIs which are deprecated or otherwise "on the chopping
  block." The libfossil public API has gathered a good deal of cruft
  over the years.
*/

/**
   @deprecated fsl_db_role_name() is easier to deal with.

   Similar to fsl_cx_db_file_ckout() and friends except that it
   applies to DB name (as opposed to DB _file_ name) implied by the
   specified role (2nd parameter). If no such role is opened, or the
   role is invalid, NULL is returned.

   If the 3rd argument is not NULL, it is set to the length, in bytes,
   of the returned string. The returned strings are NUL-terminated and
   are either static or owned by the db handle they correspond to.

   If the client does not care whether the db in question is
   actually opened, the name for the corresponding role can be
   fetched via fsl_db_role_name().

   This is the "easiest" way to figure out the DB name of the given
   role, independent of what order f's databases were opened
   (because the first-opened DB is always called "main").

   The Fossil-standard names of its primary databases are: "localdb"
   (checkout), "repository", and "configdb" (global config DB), but
   libfossil uses "ckout", "repo", and "cfg", respective. So long as
   queries use table names which unambiguously refer to a given
   database, the DB name is normally not needed. It is needed when
   creating new non-TEMP db tables and views. By default such
   tables/views would go into the "main" DB, and which one is the
   "main" db is dependent on the order the DBs are opened, so it's
   important to use the correct DB name when creating such constructs.

   Note that the role of FSL_DBROLE_TEMP is invalid here.
*/
char const * fsl_cx_db_name_for_role(fsl_cx const * const f,
                                     fsl_dbrole_e r,
                                     fsl_size_t * len);

/**
   Equivalent to `fcli_setup_v2(argc,argv,fcli.cliFlags,fcli.appHelp)`.

   @see fcli_pre_setup()
   @see fcli_setup_v2()
   @see fcli_end_of_main()
   @deprecated Its signature will change to fcli_setup_v2()'s at some point.
*/
int fcli_setup(int argc, char const * const * argv );

/** @deprecated fsl_close_scm_dbs()

   As of 2021-01-01, this functions identically to
   fsl_close_scm_dbs(). Prior to that...

   If fsl_repo_open_xxx() has been used to open a respository db,
   perhaps indirectly via opening of a checkout, this call closes that
   db and any corresponding checkout db.

   Returns 0 on success or if no repo/checkout db is opened. It may
   propagate an error from the db layer if closing/detaching the db
   fails.

   @see fsl_repo_open()
   @see fsl_repo_create()
*/
int fsl_repo_close( fsl_cx * const f );


/** @deprecated use fsl_close_scm_dbs() instead

   As of 2021-01-01, this functions identically to
   fsl_close_scm_dbs(). Prior to that...

   If fsl_ckout_open_dir() (or similar) has been used to open a
   checkout db, this call closes that db, as well as any
   corresponding repository db.

   Returns 0 on success or if no checkout db is opened. It may
   propagate an error from the db layer if closing/detaching the db
   fails. Returns FSL_RC_MISUSE if f has any transactions pending.

   This also closes the repository which was implicitly opened for the
   checkout.
*/
int fsl_ckout_close( fsl_cx * const f );

#endif /* ORG_FOSSIL_SCM_LIBFOSSIL_DEPRECATED_H_INCLUDED */
#endif /* FSL_OMIT_DEPRECATED */
