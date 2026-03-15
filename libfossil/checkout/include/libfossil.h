/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
#if !defined(ORG_FOSSIL_SCM_LIBFOSSIL_H_INCLUDED)
#define ORG_FOSSIL_SCM_LIBFOSSIL_H_INCLUDED
/*
  Copyright 2013-2024 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/
/** @file libfossil.h

  This file is the primary header for the public APIs. It includes
  various other header files. They are split into multiple headers
  primarily because my lower-end systems choke on syntax-highlighting
  them and browsing their (large) Doxygen output.

  The "amalgamation" distribution of this library compounds all of
  these headers into a single libfossil.h, and that is the way it is
  intended to be used by clients which reside outside of this
  project's own source tree.
*/

//#define FSL_OMIT_DEPRECATED

/*
   config.h MUST be included first so we can set some
   portability flags and config-dependent typedefs!
*/
#include "fossil-scm/config.h"
#include "fossil-scm/util.h"
#include "fossil-scm/core.h"
#include "fossil-scm/db.h"
#include "fossil-scm/repo.h"
#include "fossil-scm/checkout.h"
#include "fossil-scm/confdb.h"
#include "fossil-scm/hash.h"
#include "fossil-scm/auth.h"
#include "fossil-scm/vpath.h"
#include "fossil-scm/sync.h"
#include "fossil-scm/cli.h"
#include "fossil-scm/deprecated.h"

#endif
/* ORG_FOSSIL_SCM_LIBFOSSIL_H_INCLUDED */
