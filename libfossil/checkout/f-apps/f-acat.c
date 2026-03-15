/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */ 
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).

  *****************************************************************************
   A simple tool for dumping fossil blobs to stdout.
*/

#include "libfossil.h"

int main(int argc, char const * const * argv ){
  int rc = 0;
  const char * sym = NULL;
  const char * ofileName = NULL;
  FILE * ofile = NULL;
  fsl_cx * f;
  fsl_db * db;
  fsl_id_t rid = 0;
  fsl_uuid_str uuid = NULL;
  fsl_buffer blob = fsl_buffer_empty;
  bool raw = false;
  fcli_cliflag FCliFlags[] = {
    FCLI_FLAG("a","artifact","artifact-id",&sym,
              "Artifact UUID or symbolic name. "
              "Default is the first non-flag argument."),
    FCLI_FLAG("o","output","filename",&ofileName,
              "Output to the given file."),
    FCLI_FLAG_BOOL(0,"raw",&raw,
                   "Fetches blobs in raw form, which means that no "
                   "undeltification is applied (but they are decompressed, if "
                   "needed, to undo fossil's custom tagging of the "
                   "compression state)"),
    fcli_cliflag_empty_m
  };
  fcli_help_info FCliHelp = {
  "Outputs content from fossil repositories.",
  "artifact_id",
  NULL
  };

  rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;
  f = fcli_cx();
  db = fsl_needs_repo(f);
  if(!db){
    goto end;
  }
  if(fcli_has_unused_flags(0)) goto end;
  if(!sym) sym = fcli_next_arg(1);
  if(!sym){
    fcli_err_set(FSL_RC_MISUSE, "Missing artifact argument.");
    goto end;
  }
  else if(ofileName){
    ofile = fsl_fopen(ofileName, "w");
    if(!ofile){
      rc = fcli_err_set(FSL_RC_IO,
                        "Could not open file for writing: %s", ofileName);
    }
    ofileName = NULL;
    if(rc) goto end;
  }

  rc = fsl_sym_to_uuid(f, sym, FSL_SATYPE_ANY, &uuid, &rid);
  if(rc) goto end;
  FCLI_V(("Symbol [%s] resolved to [%.*s] (rid %"FSL_ID_T_PFMT")\n",
           sym, 12, uuid, rid));
  rc = raw
    ? fsl_content_raw(f, rid, &blob)
    : fsl_content_get(f, rid, &blob);
  if(!rc){
    fwrite(blob.mem, blob.used, 1, ofile ? ofile : stdout);
  }
  end:
  fsl_fclose(ofile);
  fsl_buffer_clear(&blob);
  fsl_free(uuid);
  return fcli_end_of_main(rc);
}
