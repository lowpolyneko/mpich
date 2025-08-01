/*
 * Copyright (C) by Argonne National Laboratory
 *     See COPYRIGHT in top-level directory
 */

#include "mpiimpl.h"


int MPIR_Scan_intra_smp(const void *sendbuf, void *recvbuf, MPI_Aint count,
                        MPI_Datatype datatype, MPI_Op op, MPIR_Comm * comm_ptr, int coll_attr)
{
    int mpi_errno = MPI_SUCCESS;
    MPIR_CHKLMEM_DECL();
    int rank;
    int size ATTRIBUTE((unused));
    MPIR_COMM_RANK_SIZE(comm_ptr, rank, size);
    MPI_Status status;
    void *tempbuf = NULL, *localfulldata = NULL, *prefulldata = NULL;
    MPI_Aint true_lb, true_extent, extent;
    int noneed = 1;             /* noneed=1 means no need to bcast tempbuf and
                                 * reduce tempbuf & recvbuf */

    MPIR_Type_get_true_extent_impl(datatype, &true_lb, &true_extent);

    MPIR_Datatype_get_extent_macro(datatype, extent);

    MPIR_CHKLMEM_MALLOC(tempbuf, count * (MPL_MAX(extent, true_extent)));
    tempbuf = (void *) ((char *) tempbuf - true_lb);

    /* Create prefulldata and localfulldata on local roots of all nodes */
    if (comm_ptr->node_roots_comm != NULL) {
        MPIR_CHKLMEM_MALLOC(prefulldata, count * (MPL_MAX(extent, true_extent)));
        prefulldata = (void *) ((char *) prefulldata - true_lb);

        if (comm_ptr->node_comm != NULL) {
            MPIR_CHKLMEM_MALLOC(localfulldata, count * (MPL_MAX(extent, true_extent)));
            localfulldata = (void *) ((char *) localfulldata - true_lb);
        }
    }

    /* perform intranode scan to get temporary result in recvbuf. if there is only
     * one process, just copy the raw data. */
    if (comm_ptr->node_comm != NULL) {
        mpi_errno =
            MPIR_Scan(sendbuf, recvbuf, count, datatype, op, comm_ptr->node_comm, coll_attr);
        MPIR_ERR_CHECK(mpi_errno);
    } else if (sendbuf != MPI_IN_PLACE) {
        mpi_errno = MPIR_Localcopy(sendbuf, count, datatype, recvbuf, count, datatype);
        MPIR_ERR_CHECK(mpi_errno);
    }

    /* get result from local node's last processor which
     * contains the reduce result of the whole node. Name it as
     * localfulldata. For example, localfulldata from node 1 contains
     * reduced data of rank 1,2,3. */
    if (comm_ptr->node_roots_comm != NULL && comm_ptr->node_comm != NULL) {
        mpi_errno = MPIC_Recv(localfulldata, count, datatype,
                              comm_ptr->node_comm->local_size - 1, MPIR_SCAN_TAG,
                              comm_ptr->node_comm, &status);
        MPIR_ERR_CHECK(mpi_errno);
    } else if (comm_ptr->node_roots_comm == NULL &&
               comm_ptr->node_comm != NULL &&
               MPIR_Get_intranode_rank(comm_ptr, rank) == comm_ptr->node_comm->local_size - 1) {
        mpi_errno = MPIC_Send(recvbuf, count, datatype,
                              0, MPIR_SCAN_TAG, comm_ptr->node_comm, coll_attr);
        MPIR_ERR_CHECK(mpi_errno);
    } else if (comm_ptr->node_roots_comm != NULL) {
        localfulldata = recvbuf;
    }

    /* do scan on localfulldata to prefulldata. for example,
     * prefulldata on rank 4 contains reduce result of ranks
     * 1,2,3,4,5,6. it will be sent to rank 7 which is the
     * main process of node 3. */
    if (comm_ptr->node_roots_comm != NULL) {
        mpi_errno = MPIR_Scan(localfulldata, prefulldata, count, datatype,
                              op, comm_ptr->node_roots_comm, coll_attr);
        MPIR_ERR_CHECK(mpi_errno);

        if (MPIR_Get_internode_rank(comm_ptr, rank) != comm_ptr->node_roots_comm->local_size - 1) {
            mpi_errno = MPIC_Send(prefulldata, count, datatype,
                                  MPIR_Get_internode_rank(comm_ptr, rank) + 1,
                                  MPIR_SCAN_TAG, comm_ptr->node_roots_comm, coll_attr);
            MPIR_ERR_CHECK(mpi_errno);
        }
        if (MPIR_Get_internode_rank(comm_ptr, rank) != 0) {
            mpi_errno = MPIC_Recv(tempbuf, count, datatype,
                                  MPIR_Get_internode_rank(comm_ptr, rank) - 1,
                                  MPIR_SCAN_TAG, comm_ptr->node_roots_comm, &status);
            noneed = 0;
            MPIR_ERR_CHECK(mpi_errno);
        }
    }

    /* now tempbuf contains all the data needed to get the correct
     * scan result. for example, to node 3, it will have reduce result
     * of rank 1,2,3,4,5,6 in tempbuf.
     * then we should broadcast this result in the local node, and
     * reduce it with recvbuf to get final result if necessary. */

    if (comm_ptr->node_comm != NULL) {
        mpi_errno = MPIR_Bcast(&noneed, 1, MPIR_INT_INTERNAL, 0, comm_ptr->node_comm, coll_attr);
        MPIR_ERR_CHECK(mpi_errno);
    }

    if (noneed == 0) {
        if (comm_ptr->node_comm != NULL) {
            mpi_errno = MPIR_Bcast(tempbuf, count, datatype, 0, comm_ptr->node_comm, coll_attr);
            MPIR_ERR_CHECK(mpi_errno);
        }

        mpi_errno = MPIR_Reduce_local(tempbuf, recvbuf, count, datatype, op);
    }

  fn_exit:
    MPIR_CHKLMEM_FREEALL();
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}
