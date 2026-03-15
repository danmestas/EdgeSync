/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/

#include "libfossil.h"
#include "fossil-scm/internal.h"

int main(int argc, const char * const * argv ){
  typedef double julian_date_pseudotype /* only for output purposes */;
  fcli_cliflag FCliFlags[] = {
    fcli_cliflag_empty_m
  };
  fcli_help_info FCliHelp = {
  "Shows the sizeof() values for various fsl API types.",
  NULL, NULL
  };
  fcli.config.checkoutDir = NULL;
  int rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;
#define T(X) printf("\tstrlen(fsl_schema_"#X") = %u\n", (unsigned)fsl_strlen(fsl_schema_##X()))
  puts("SQL schema sizes:");
  T(config);
  T(repo1);
  T(repo2);
  T(ckout);
  T(ticket_reports);
#undef T

  puts("\nsizeof() values and format strings for various numeric types:");
#define FT(X,FMT) printf("%-6d "#X" fmt=%%%s\n", (int)sizeof(X),FMT)
#define T(X) printf("%-6d "#X"\n", (int)sizeof(X))
  FT(int32_t,PRIi32);
  FT(uint32_t,PRIu32);
  FT(int64_t,PRIi64);
  FT(uint64_t,PRIu64);
  FT(fsl_int_t,FSL_INT_T_PFMT);
  FT(fsl_id_t,FSL_ID_T_PFMT);
  FT(fsl_size_t,FSL_SIZE_T_PFMT);
  FT(julian_date_pseudotype,FSL_JULIAN_T_PFMT);
  T(bool);
  T(long);
#undef FT

  puts("\nsizeof() values for various structs:");
  T(fsl__bccache);
  T(fsl__bccache_line);
  T(fsl__mcache);
  T(fsl__pq);
  T(fsl__pq_entry);
  T(fsl__ptl);
  T(fsl__ptl_line);
  T(fsl__xfer);

  T(fsl_allocator);
  T(fsl_buffer);
  T(fsl_card_F);
  T(fsl_card_J);
  T(fsl_card_Q);
  T(fsl_card_T);
  T(fsl_cx);
  T(fsl_cx_config);
  T(fsl_db);
  T(fsl_deck);
  T(fsl_error);
  T(fsl_finalizer);
  T(fsl_fstat);
  T(fsl_id_bag);
  T(fsl_list);
  T(fsl_md5_cx);
  T(fsl_outputer);
  T(fsl_pathfinder);
  T(fsl_sc);
  T(fsl_sha1_cx);
  T(fsl_sha3_cx);
  T(fsl_state);
  T(fsl_stmt);
  T(fsl_timer);
  T(fsl_uperm);
  T(fsl_url);

  T(fcli_cliflag);
#undef T
  end:
  return fcli_end_of_main(rc);
}
