/*
 * Copyright (C) by Argonne National Laboratory
 *     See COPYRIGHT in top-level directory
 */

#include "mpiimpl.h"

/* Algorithm: MPI_Barrier
 *
 * We use the dissemination algorithm described in:
 * Debra Hensgen, Raphael Finkel, and Udi Manbet, "Two Algorithms for
 * Barrier Synchronization," International Journal of Parallel
 * Programming, 17(1):1-17, 1988.
 *
 * It uses ceiling(lgp) steps. In step k, 0 <= k <= (ceiling(lgp)-1),
 * process i sends to process (i + 2^k) % p and receives from process
 * (i - 2^k + p) % p.
 */
int MPIR_Barrier_intra_dissemination(MPIR_Comm * comm_ptr, int coll_attr)
{
    int size, rank, src, dst, mask, mpi_errno = MPI_SUCCESS;

    MPIR_COMM_RANK_SIZE(comm_ptr, rank, size);

    mask = 0x1;
    while (mask < size) {
        dst = (rank + mask) % size;
        src = (rank - mask + size) % size;
        mpi_errno = MPIC_Sendrecv(NULL, 0, MPIR_BYTE_INTERNAL, dst,
                                  MPIR_BARRIER_TAG, NULL, 0, MPIR_BYTE_INTERNAL,
                                  src, MPIR_BARRIER_TAG, comm_ptr, MPI_STATUS_IGNORE, coll_attr);
        MPIR_ERR_CHECK(mpi_errno);
        mask <<= 1;
    }

  fn_exit:
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

/* Algorithm: high radix dissemination
 * Similar to dissemination algorithm, but generalized with high radix k
 */
int MPIR_Barrier_intra_k_dissemination(MPIR_Comm * comm, int k, int coll_attr)
{
    int mpi_errno = MPI_SUCCESS;
    int i, j, nranks, rank;
    int p_of_k;                 /* minimum power of k that is greater than or equal to number of ranks */
    int shift, to, from;
    int nphases = 0;
    MPIR_Request *sreqs[MAX_RADIX], *rreqs[MAX_RADIX * 2];
    MPIR_Request **send_reqs = NULL, **recv_reqs = NULL;

    MPIR_COMM_RANK_SIZE(comm, rank, nranks);

    if (nranks == 1)
        goto fn_exit;

    if (nranks < k)
        k = nranks;

    if (k == 2) {
        return MPIR_Barrier_intra_dissemination(comm, coll_attr);
    }

    /* If k value is greater than the maximum radix defined by MAX_RADIX macro,
     * we allocate memory for requests here. Otherwise we use the requests defined
     * in the communicator for allreduce/barrier recexch */
    if (k > MAX_RADIX) {
        recv_reqs =
            (MPIR_Request **) MPL_malloc((k - 1) * 2 * sizeof(MPIR_Request *), MPL_MEM_BUFFER);
        MPIR_ERR_CHKANDJUMP(!recv_reqs, mpi_errno, MPI_ERR_OTHER, "**nomem");
        send_reqs = (MPIR_Request **) MPL_malloc((k - 1) * sizeof(MPIR_Request *), MPL_MEM_BUFFER);
        MPIR_ERR_CHKANDJUMP(!send_reqs, mpi_errno, MPI_ERR_OTHER, "**nomem");
    } else {
        send_reqs = sreqs;
        recv_reqs = rreqs;
    }

    p_of_k = 1;
    while (p_of_k < nranks) {
        p_of_k *= k;
        nphases++;
    }

    shift = 1;
    for (i = 0; i < nphases; i++) {
        for (j = 1; j < k; j++) {
            to = (rank + j * shift) % nranks;
            from = (nranks + (rank - j * shift)) % nranks;
            while (from < 0)
                from += nranks;
            MPIR_Assert(from >= 0 && from < nranks);
            MPIR_Assert(to >= 0 && to < nranks);

            /* recv from (k-1) nbrs */
            mpi_errno =
                MPIC_Irecv(NULL, 0, MPIR_BYTE_INTERNAL, from, MPIR_BARRIER_TAG, comm,
                           &recv_reqs[(j - 1) + ((k - 1) * (i & 1))]);
            MPIR_ERR_CHECK(mpi_errno);
            /* wait on recvs from prev phase */
            if (i > 0 && j == 1) {
                mpi_errno =
                    MPIC_Waitall(k - 1, &recv_reqs[((k - 1) * ((i - 1) & 1))], MPI_STATUSES_IGNORE);
                MPIR_ERR_CHECK(mpi_errno);
            }

            mpi_errno =
                MPIC_Isend(NULL, 0, MPIR_BYTE_INTERNAL, to, MPIR_BARRIER_TAG, comm,
                           &send_reqs[j - 1], coll_attr);
            MPIR_ERR_CHECK(mpi_errno);
        }
        mpi_errno = MPIC_Waitall(k - 1, send_reqs, MPI_STATUSES_IGNORE);
        MPIR_ERR_CHECK(mpi_errno);
        shift *= k;
    }

    mpi_errno =
        MPIC_Waitall(k - 1, recv_reqs + ((k - 1) * ((nphases - 1) & 1)), MPI_STATUSES_IGNORE);
    MPIR_ERR_CHECK(mpi_errno);

  fn_exit:
    if (k > MAX_RADIX) {
        MPL_free(recv_reqs);
        MPL_free(send_reqs);
    }
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}
