! -*- f90 -*-
!
! Copyright (c) 2009-2012 Cisco Systems, Inc.  All rights reserved.
! Copyright (c) 2009-2012 Los Alamos National Security, LLC.
!                         All rights reserved.
! $COPYRIGHT$

subroutine PMPI_Intercomm_merge_f08(intercomm,high,newintracomm,ierror)
   use :: mpi_f08_types, only : MPI_Comm
   use :: mpi_f08, only : ompi_intercomm_merge_f
   implicit none
   TYPE(MPI_Comm), INTENT(IN) :: intercomm
   LOGICAL, INTENT(IN) :: high
   TYPE(MPI_Comm), INTENT(OUT) :: newintracomm
   INTEGER, OPTIONAL, INTENT(OUT) :: ierror
   integer :: c_ierror

   call ompi_intercomm_merge_f(intercomm%MPI_VAL,high,newintracomm%MPI_VAL,c_ierror)
   if (present(ierror)) ierror = c_ierror

end subroutine PMPI_Intercomm_merge_f08
