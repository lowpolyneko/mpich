/*
 * Copyright (C) by Argonne National Laboratory
 *     See COPYRIGHT in top-level directory
 */

#include "mpiimpl.h"

/* This function implements a binomial tree reduce.

   Cost = lgp.alpha + n.lgp.beta + n.lgp.gamma
 */
int MPIR_Reduce_intra_binomial(const void *sendbuf,
                               void *recvbuf,
                               MPI_Aint count,
                               MPI_Datatype datatype,
                               MPI_Op op, int root, MPIR_Comm * comm_ptr, int coll_attr)
{
    int mpi_errno = MPI_SUCCESS;
    MPI_Status status;
    int comm_size, rank, is_commutative;
    int mask, relrank, source, lroot;
    MPI_Aint true_lb, true_extent, extent;
    void *tmp_buf;
    MPIR_CHKLMEM_DECL();

    MPIR_COMM_RANK_SIZE(comm_ptr, rank, comm_size);

    /* Create a temporary buffer */

    MPIR_Type_get_true_extent_impl(datatype, &true_lb, &true_extent);
    MPIR_Datatype_get_extent_macro(datatype, extent);

    is_commutative = MPIR_Op_is_commutative(op);

    MPIR_CHKLMEM_MALLOC(tmp_buf, count * (MPL_MAX(extent, true_extent)));
    /* adjust for potential negative lower bound in datatype */
    tmp_buf = (void *) ((char *) tmp_buf - true_lb);

    /* If I'm not the root, then my recvbuf may not be valid, therefore
     * I have to allocate a temporary one */
    if (rank != root) {
        MPIR_CHKLMEM_MALLOC(recvbuf, count * (MPL_MAX(extent, true_extent)));
        recvbuf = (void *) ((char *) recvbuf - true_lb);
    }

    if ((rank != root) || (sendbuf != MPI_IN_PLACE)) {
        mpi_errno = MPIR_Localcopy(sendbuf, count, datatype, recvbuf, count, datatype);
        MPIR_ERR_CHECK(mpi_errno);
    }

    /* This code is from MPICH-1. */

    /* Here's the algorithm.  Relative to the root, look at the bit pattern in
     * my rank.  Starting from the right (lsb), if the bit is 1, send to
     * the node with that bit zero and exit; if the bit is 0, receive from the
     * node with that bit set and combine (as long as that node is within the
     * group)
     *
     * Note that by receiving with source selection, we guarantee that we get
     * the same bits with the same input.  If we allowed the parent to receive
     * the children in any order, then timing differences could cause different
     * results (roundoff error, over/underflows in some cases, etc).
     *
     * Because of the way these are ordered, if root is 0, then this is correct
     * for both commutative and non-commutative operations.  If root is not
     * 0, then for non-commutative, we use a root of zero and then send
     * the result to the root.  To see this, note that the ordering is
     * mask = 1: (ab)(cd)(ef)(gh)            (odds send to evens)
     * mask = 2: ((ab)(cd))((ef)(gh))        (3,6 send to 0,4)
     * mask = 4: (((ab)(cd))((ef)(gh)))      (4 sends to 0)
     *
     * Comments on buffering.
     * If the datatype is not contiguous, we still need to pass contiguous
     * data to the user routine.
     * In this case, we should make a copy of the data in some format,
     * and send/operate on that.
     *
     * In general, we can't use MPI_PACK, because the alignment of that
     * is rather vague, and the data may not be re-usable.  What we actually
     * need is a "squeeze" operation that removes the skips.
     */
    mask = 0x1;
    if (is_commutative)
        lroot = root;
    else
        lroot = 0;
    relrank = (rank - lroot + comm_size) % comm_size;

    while (/*(mask & relrank) == 0 && */ mask < comm_size) {
        /* Receive */
        if ((mask & relrank) == 0) {
            source = (relrank | mask);
            if (source < comm_size) {
                source = (source + lroot) % comm_size;
                mpi_errno = MPIC_Recv(tmp_buf, count, datatype, source,
                                      MPIR_REDUCE_TAG, comm_ptr, &status);
                MPIR_ERR_CHECK(mpi_errno);

                /* The sender is above us, so the received buffer must be
                 * the second argument (in the noncommutative case). */
                if (is_commutative) {
                    mpi_errno = MPIR_Reduce_local(tmp_buf, recvbuf, count, datatype, op);
                    MPIR_ERR_CHECK(mpi_errno);
                } else {
                    mpi_errno = MPIR_Reduce_local(recvbuf, tmp_buf, count, datatype, op);
                    MPIR_ERR_CHECK(mpi_errno);

                    mpi_errno = MPIR_Localcopy(tmp_buf, count, datatype, recvbuf, count, datatype);
                    MPIR_ERR_CHECK(mpi_errno);
                }
            }
        } else {
            /* I've received all that I'm going to.  Send my result to
             * my parent */
            source = ((relrank & (~mask)) + lroot) % comm_size;
            mpi_errno = MPIC_Send(recvbuf, count, datatype,
                                  source, MPIR_REDUCE_TAG, comm_ptr, coll_attr);
            MPIR_ERR_CHECK(mpi_errno);
            break;
        }
        mask <<= 1;
    }

    if (!is_commutative && (root != 0)) {
        if (rank == 0) {
            mpi_errno = MPIC_Send(recvbuf, count, datatype, root,
                                  MPIR_REDUCE_TAG, comm_ptr, coll_attr);
        } else if (rank == root) {
            mpi_errno = MPIC_Recv(recvbuf, count, datatype, 0, MPIR_REDUCE_TAG, comm_ptr, &status);
        }
        MPIR_ERR_CHECK(mpi_errno);
    }

  fn_exit:
    MPIR_CHKLMEM_FREEALL();
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}
