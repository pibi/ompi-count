/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart, 
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2012 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2013      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "ompi_config.h"

#include "ompi/mpi/fortran/mpif-h/bindings.h"

#if OPAL_HAVE_WEAK_SYMBOLS && OMPI_PROFILE_LAYER
#pragma weak PMPI_TYPE_SIZE_X = ompi_type_size_x_f
#pragma weak pmpi_type_size_x = ompi_type_size_x_f
#pragma weak pmpi_type_size_x_ = ompi_type_size_x_f
#pragma weak pmpi_type_size_x__ = ompi_type_size_x_f

#pragma weak PMPI_type_size_x_f = ompi_type_size_x_f
#pragma weak PMPI_type_size_x_f08 = ompi_type_size_x_f
#elif OMPI_PROFILE_LAYER
OMPI_GENERATE_F77_BINDINGS (PMPI_TYPE_SIZE_X,
                           pmpi_type_size_x,
                           pmpi_type_size_x_,
                           pmpi_type_size_x__,
                           pompi_type_size_x_f,
                           (MPI_Fint *type, MPI_Count *size, MPI_Fint *ierr),
                           (type, size, ierr) )
#endif

#if OPAL_HAVE_WEAK_SYMBOLS
#pragma weak MPI_TYPE_SIZE_X = ompi_type_size_x_f
#pragma weak mpi_type_size_x = ompi_type_size_x_f
#pragma weak mpi_type_size_x_ = ompi_type_size_x_f
#pragma weak mpi_type_size_x__ = ompi_type_size_x_f

#pragma weak MPI_type_size_x_f = ompi_type_size_x_f
#pragma weak MPI_type_size_x_f08 = ompi_type_size_x_f
#endif

#if ! OPAL_HAVE_WEAK_SYMBOLS && ! OMPI_PROFILE_LAYER
OMPI_GENERATE_F77_BINDINGS (MPI_TYPE_SIZE_X,
                           mpi_type_size_x,
                           mpi_type_size_x_,
                           mpi_type_size_x__,
                           ompi_type_size_x_f,
                           (MPI_Fint *type, MPI_Count *size, MPI_Fint *ierr),
                           (type, size, ierr) )
#endif


#if OMPI_PROFILE_LAYER && ! OPAL_HAVE_WEAK_SYMBOLS
#include "ompi/mpi/fortran/mpif-h/profile/defines.h"
#endif

void ompi_type_size_x_f(MPI_Fint *type, MPI_Count *size, MPI_Fint *ierr)
{
    int c_ierr;
    MPI_Datatype c_type = MPI_Type_f2c(*type);
    OMPI_SINGLE_NAME_DECL(size);

    c_ierr = MPI_Type_size_x(c_type, size);
    if (NULL != ierr) *ierr = OMPI_INT_2_FINT(c_ierr);
}
