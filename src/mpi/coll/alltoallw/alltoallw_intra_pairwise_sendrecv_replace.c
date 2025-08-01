/*
 * Copyright (C) by Argonne National Laboratory
 *     See COPYRIGHT in top-level directory
 */

#include "mpiimpl.h"

/* Algorithm: Inplace Alltoallw
 *
 * We use pair-wise sendrecv_replace in order to conserve memory usage, which
 * is keeping with the spirit of the MPI-2.2 Standard.  But because of this
 * approach all processes must agree on the global schedule of sendrecv_replace
 * operations to avoid deadlock.
 *
 * Note that this is not an especially efficient algorithm in terms of time and
 * there will be multiple repeated malloc/free's rather than maintaining a
 * single buffer across the whole loop.  Something like MADRE is probably the
 * best solution for the MPI_IN_PLACE scenario.
 */
int MPIR_Alltoallw_intra_pairwise_sendrecv_replace(const void *sendbuf, const MPI_Aint sendcounts[],
                                                   const MPI_Aint sdispls[],
                                                   const MPI_Datatype sendtypes[], void *recvbuf,
                                                   const MPI_Aint recvcounts[],
                                                   const MPI_Aint rdispls[],
                                                   const MPI_Datatype recvtypes[],
                                                   MPIR_Comm * comm_ptr, int coll_attr)
{
    int comm_size, i, j;
    int mpi_errno = MPI_SUCCESS;
    MPI_Status status;
    int rank;

    MPIR_COMM_RANK_SIZE(comm_ptr, rank, comm_size);

#ifdef HAVE_ERROR_CHECKING
    MPIR_Assert(sendbuf == MPI_IN_PLACE);
#endif

    /* We use pair-wise sendrecv_replace in order to conserve memory usage,
     * which is keeping with the spirit of the MPI-2.2 Standard.  But
     * because of this approach all processes must agree on the global
     * schedule of sendrecv_replace operations to avoid deadlock.
     *
     * Note that this is not an especially efficient algorithm in terms of
     * time and there will be multiple repeated malloc/free's rather than
     * maintaining a single buffer across the whole loop.  Something like
     * MADRE is probably the best solution for the MPI_IN_PLACE scenario. */
    for (i = 0; i < comm_size; ++i) {
        /* start inner loop at i to avoid re-exchanging data */
        for (j = i; j < comm_size; ++j) {
            if (rank == i) {
                /* also covers the (rank == i && rank == j) case */
                mpi_errno = MPIC_Sendrecv_replace(((char *) recvbuf + rdispls[j]),
                                                  recvcounts[j], recvtypes[j],
                                                  j, MPIR_ALLTOALLW_TAG,
                                                  j, MPIR_ALLTOALLW_TAG,
                                                  comm_ptr, &status, coll_attr);
                MPIR_ERR_CHECK(mpi_errno);
            } else if (rank == j) {
                /* same as above with i/j args reversed */
                mpi_errno = MPIC_Sendrecv_replace(((char *) recvbuf + rdispls[i]),
                                                  recvcounts[i], recvtypes[i],
                                                  i, MPIR_ALLTOALLW_TAG,
                                                  i, MPIR_ALLTOALLW_TAG,
                                                  comm_ptr, &status, coll_attr);
                MPIR_ERR_CHECK(mpi_errno);
            }
        }
    }

  fn_exit:
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}
