/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2012-2013 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "ompi/mpit/mpit-internal.h"

static const char FUNC_NAME[] = "MPI_T_pvar_reset";

int MPI_T_pvar_reset(MPI_T_pvar_session session, MPI_T_pvar_handle handle)
{
    int ret;

    if (!mpit_is_initialized ()) {
        return MPI_T_ERR_NOT_INITIALIZED;
    }

    mpit_lock ();

    if (MPI_T_PVAR_ALL_HANDLES == handle) {
        OPAL_LIST_FOREACH(handle, &session->handles, mca_base_pvar_handle_t) {
            /* Per MPI 3.0: ignore read-only variables when resetting all
               handles. */
            if (!mca_base_pvar_is_readonly (handle->pvar) &&
                MPI_SUCCESS != mca_base_pvar_handle_reset (handle)) {
                ret = MPI_T_ERR_PVAR_NO_WRITE;
            }
        }
    } else {
        ret = mca_base_pvar_handle_reset (handle);
    }

    mpit_unlock ();

    return ret;
}
