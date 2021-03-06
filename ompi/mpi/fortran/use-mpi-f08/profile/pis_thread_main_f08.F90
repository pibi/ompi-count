! -*- f90 -*-
!
! Copyright (c) 2010-2012 Cisco Systems, Inc.  All rights reserved.
! Copyright (c) 2009-2012 Los Alamos National Security, LLC.
!               All Rights reserved.
! $COPYRIGHT$

subroutine PMPI_Is_thread_main_f08(flag,ierror)
   use :: mpi_f08, only : ompi_is_thread_main_f
   implicit none
   LOGICAL, INTENT(OUT) :: flag
   INTEGER, OPTIONAL, INTENT(OUT) :: ierror
   integer :: c_ierror

   call ompi_is_thread_main_f(flag,c_ierror)
   if (present(ierror)) ierror = c_ierror

end subroutine PMPI_Is_thread_main_f08
