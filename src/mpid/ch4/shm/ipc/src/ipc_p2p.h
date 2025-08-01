/*
 * Copyright (C) by Argonne National Laboratory
 *     See COPYRIGHT in top-level directory
 */

#ifndef IPC_P2P_H_INCLUDED
#define IPC_P2P_H_INCLUDED

#include "ch4_impl.h"
#include "mpidimpl.h"
#include "ipc_pre.h"
#include "ipc_types.h"
#include "../xpmem/xpmem_post.h"
#include "../cma/cma_post.h"
#include "../gpu/gpu_post.h"

/* Generic IPC protocols for P2P. */

/* Generic sender-initialized LMT routine with contig send buffer.
 *
 * If the send buffer is noncontiguous the submodule can first pack the
 * data into a temporary buffer and use the temporary buffer as the send
 * buffer with this call. The sender gets the memory attributes of the
 * specified buffer (which include IPC type and memory handle), and sends
 * to the receiver. The receiver will then open the remote memory handle
 * and perform direct data transfer.
 */

int MPIDI_IPC_complete(MPIR_Request * req, MPIDI_IPCI_type_t ipc_type);
int MPIDI_IPC_rndv_cb(MPIR_Request * rreq);
int MPIDI_IPC_do_cts(MPIR_Request * rreq);
int MPIDI_IPC_ack_target_msg_cb(void *am_hdr, void *data, MPI_Aint in_data_sz,
                                uint32_t attr, MPIR_Request ** req);
int MPIDI_IPC_write_target_msg_cb(void *am_hdr, void *data, MPI_Aint in_data_sz,
                                  uint32_t attr, MPIR_Request ** req);

MPL_STATIC_INLINE_PREFIX bool MPIDI_IPCI_has_ipc(void)
{
#ifdef MPIDI_CH4_SHM_ENABLE_GPU
    return true;
#elif defined(MPIDI_CH4_SHM_ENABLE_XPMEM)
    return true;
#elif defined(MPIDI_CH4_SHM_ENABLE_CMA)
    return true;
#else
    return false;
#endif
}

/* use this macro to prevent unused label warning in case no IPC is enabled */
#define MPIDI_IPCI_NO_IPC_EXIT if (!MPIDI_IPCI_has_ipc()) goto fn_exit

MPL_STATIC_INLINE_PREFIX int MPIDI_IPCI_get_ipc_attr(const void *buf, MPI_Aint count,
                                                     MPI_Datatype datatype,
                                                     int remote_rank, MPIR_Comm * comm,
                                                     int msg_hdr_sz,
                                                     MPIDI_IPCI_ipc_attr_t * ipc_attr)
{
    int mpi_errno = MPI_SUCCESS;

    ipc_attr->ipc_type = MPIDI_IPCI_TYPE__NONE;
    MPIDI_IPCI_NO_IPC_EXIT;

    int dt_contig;
    MPIR_Datatype_is_contig(datatype, &dt_contig);
    if (!dt_contig) {
        int flattened_sz;
        void *flattened_dt;
        MPIR_Datatype_get_flattened(datatype, &flattened_dt, &flattened_sz);
        if (msg_hdr_sz + sizeof(MPIDI_IPC_hdr) + flattened_sz > MPIDI_POSIX_am_hdr_max_sz()) {
            goto fn_exit;
        }
    }
#ifdef MPIDI_CH4_SHM_ENABLE_GPU
    mpi_errno = MPIDI_GPU_get_ipc_attr(buf, count, datatype, remote_rank, comm, ipc_attr);
    MPIR_ERR_CHECK(mpi_errno);
    if (ipc_attr->ipc_type == MPIDI_IPCI_TYPE__SKIP) {
        /* GPU IPC is not supported but it is still a device memory,
         * we can't do shared memory IPC either, so skip to fn_exit. */
        ipc_attr->ipc_type = MPIDI_IPCI_TYPE__NONE;
        goto fn_exit;
    }
    if (ipc_attr->ipc_type != MPIDI_IPCI_TYPE__NONE) {
        goto fn_exit;
    }
#endif
#ifdef MPIDI_CH4_SHM_ENABLE_XPMEM
    mpi_errno = MPIDI_XPMEM_get_ipc_attr(buf, count, datatype, ipc_attr);
    MPIR_ERR_CHECK(mpi_errno);
    if (ipc_attr->ipc_type != MPIDI_IPCI_TYPE__NONE) {
        goto fn_exit;
    }
#endif
#ifdef MPIDI_CH4_SHM_ENABLE_CMA
    mpi_errno = MPIDI_CMA_get_ipc_attr(buf, count, datatype, ipc_attr);
    MPIR_ERR_CHECK(mpi_errno);
    if (ipc_attr->ipc_type != MPIDI_IPCI_TYPE__NONE) {
        goto fn_exit;
    }
#endif

  fn_exit:
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

MPL_STATIC_INLINE_PREFIX int MPIDI_IPCI_prepare_ipc_hdr(MPIDI_IPCI_ipc_attr_t * ipc_attr,
                                                        MPI_Aint count, MPI_Datatype datatype,
                                                        int msg_hdr_sz, MPIR_Request * req,
                                                        void **hdr_out, MPI_Aint * hdr_sz_out)
{
    int mpi_errno = MPI_SUCCESS;
    MPIR_CHKPMEM_DECL();

    int is_contig;
    MPIR_Datatype_is_contig(datatype, &is_contig);

    int flattened_sz = 0;
    void *flattened_dt;
    if (!is_contig) {
        MPIR_Datatype_get_flattened(datatype, &flattened_dt, &flattened_sz);
    }

    void *hdr;
    MPI_Aint hdr_sz;
    hdr_sz = msg_hdr_sz + sizeof(MPIDI_IPC_hdr) + flattened_sz;
    MPIR_CHKPMEM_MALLOC(hdr, hdr_sz, MPL_MEM_OTHER);

    MPIDI_IPC_hdr *ipc_hdr = (void *) ((char *) hdr + msg_hdr_sz);
    ipc_hdr->ipc_type = ipc_attr->ipc_type;
    ipc_hdr->is_contig = is_contig;
    ipc_hdr->flattened_sz = flattened_sz;
    ipc_hdr->count = count;

    if (is_contig) {
        MPI_Aint data_sz;
        MPIR_Datatype_get_size_macro(datatype, data_sz);
        /* contig case set count to number of bytes */
        ipc_hdr->count = count * data_sz;
    } else {
        memcpy(ipc_hdr + 1, flattened_dt, flattened_sz);
        /* noncontig case count is the count of datatypes */
        ipc_hdr->count = count;
    }

    switch (ipc_attr->ipc_type) {
#ifdef MPIDI_CH4_SHM_ENABLE_XPMEM
        case MPIDI_IPCI_TYPE__XPMEM:
            MPIDI_XPMEM_fill_ipc_handle(ipc_attr, &(ipc_hdr->ipc_handle));
            break;
#endif
#ifdef MPIDI_CH4_SHM_ENABLE_CMA
        case MPIDI_IPCI_TYPE__CMA:
            MPIDI_CMA_fill_ipc_handle(ipc_attr, &(ipc_hdr->ipc_handle));
            break;
#endif
#ifdef MPIDI_CH4_SHM_ENABLE_GPU
        case MPIDI_IPCI_TYPE__GPU:
            MPIDI_GPU_fill_ipc_handle(ipc_attr, &(ipc_hdr->ipc_handle), req);
            break;
#endif
        default:
            MPIR_Assert(0);
            break;
    }

    *hdr_out = hdr;
    *hdr_sz_out = hdr_sz;

  fn_exit:
    return mpi_errno;
  fn_fail:
    MPIR_CHKPMEM_REAP();
    goto fn_exit;
}

MPL_STATIC_INLINE_PREFIX int MPIDI_IPCI_send_lmt(const void *buf, MPI_Aint count,
                                                 MPI_Datatype datatype,
                                                 int rank, int tag, MPIR_Comm * comm,
                                                 int context_offset, MPIDI_av_entry_t * addr,
                                                 MPIDI_IPCI_ipc_attr_t * ipc_attr,
                                                 int vci_src, int vci_dst, MPIR_Request ** request,
                                                 int coll_attr)
{
    int mpi_errno = MPI_SUCCESS;
    MPIR_Request *sreq = NULL;
    void *hdr = NULL;
    MPI_Aint hdr_sz;

    MPIR_FUNC_ENTER;

    /* Create send request */
    MPIR_Datatype_add_ref_if_not_builtin(datatype);
    sreq = MPIDIG_request_create(MPIR_REQUEST_KIND__SEND, 2, vci_src, vci_dst);
    MPIR_ERR_CHKANDSTMT((sreq) == NULL, mpi_errno, MPIX_ERR_NOREQ, goto fn_fail, "**nomemreq");
    *request = sreq;
    sreq->comm = comm;
    MPIR_Comm_add_ref(comm);
    MPIDIG_REQUEST(sreq, buffer) = (void *) buf;
    MPIDIG_REQUEST(sreq, datatype) = datatype;
    MPIDIG_REQUEST(sreq, count) = count;
    MPIDIG_REQUEST(sreq, u.ipc.peer_rank) = rank;
    MPIDIG_REQUEST(sreq, u.ipc.peer_req) = NULL;
    MPIDIG_REQUEST(sreq, u.ipc.src_dt_ptr) = NULL;
    MPIDI_SHM_REQUEST(sreq, ipc.ipc_type) = ipc_attr->ipc_type;

    /* Allocate am_hdr and fill ipc_hdr */
    mpi_errno = MPIDI_IPCI_prepare_ipc_hdr(ipc_attr, count, datatype, sizeof(MPIDIG_hdr_t),
                                           sreq, &hdr, &hdr_sz);
    MPIR_ERR_CHECK(mpi_errno);

    /* Fill am_hdr */
    MPIDIG_hdr_t *am_hdr = hdr;

    MPI_Aint data_sz;
    MPIR_Datatype_get_size_macro(datatype, data_sz);
    data_sz *= count;

    /* message matching info */
    am_hdr->src_rank = comm->rank;
    am_hdr->tag = tag;
    am_hdr->context_id = comm->context_id + context_offset;
    am_hdr->data_sz = data_sz;
    am_hdr->rndv_hdr_sz = hdr_sz - sizeof(MPIDIG_hdr_t);
    am_hdr->sreq_ptr = sreq;
    am_hdr->error_bits = coll_attr;
    am_hdr->flags = MPIDIG_AM_SEND_FLAGS_NONE;
    MPIDIG_AM_SEND_SET_RNDV(am_hdr->flags, MPIDIG_RNDV_IPC);

    CH4_CALL(am_send_hdr(rank, comm, MPIDIG_SEND, hdr, hdr_sz, vci_src, vci_dst),
             1 /* is_local */ , mpi_errno);
    MPIR_ERR_CHECK(mpi_errno);
    MPIR_REQUEST_SET_INFO(sreq, "MPIDI_IPCI_send_lmt: rank=%d, tag=%d, data_sz=%ld\n", rank, tag,
                          data_sz);

  fn_exit:
    MPL_free(hdr);
    MPIR_FUNC_EXIT;
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

MPL_STATIC_INLINE_PREFIX int MPIDI_IPCI_send_rndv(const void *buf, MPI_Aint count,
                                                  MPI_Datatype datatype,
                                                  int rank, int tag, MPIR_Comm * comm,
                                                  int context_offset, MPIDI_av_entry_t * addr,
                                                  int vci_src, int vci_dst, MPIR_Request ** request,
                                                  int coll_attr)
{
    int mpi_errno = MPI_SUCCESS;
    MPIR_Request *sreq = NULL;
    MPIR_FUNC_ENTER;

    /* Create send request */
    MPIR_Datatype_add_ref_if_not_builtin(datatype);
    sreq = MPIDIG_request_create(MPIR_REQUEST_KIND__SEND, 2, vci_src, vci_dst);
    MPIR_ERR_CHKANDSTMT((sreq) == NULL, mpi_errno, MPIX_ERR_NOREQ, goto fn_fail, "**nomemreq");
    *request = sreq;
    sreq->comm = comm;
    MPIR_Comm_add_ref(comm);
    MPIDIG_REQUEST(sreq, buffer) = (void *) buf;
    MPIDIG_REQUEST(sreq, datatype) = datatype;
    MPIDIG_REQUEST(sreq, count) = count;
    MPIDIG_REQUEST(sreq, u.send.dest) = rank;
    /* needed in case of IPC write */
    MPIDI_SHM_REQUEST(sreq, ipc.ipc_type) = MPIDI_IPCI_TYPE__NONE;

    /* Fill am hdr */
    MPIDIG_hdr_t hdr;

    MPI_Aint data_sz;
    MPIR_Datatype_get_size_macro(datatype, data_sz);
    data_sz *= count;

    /* message matching info */
    hdr.src_rank = comm->rank;
    hdr.tag = tag;
    hdr.context_id = comm->context_id + context_offset;
    hdr.data_sz = data_sz;
    hdr.rndv_hdr_sz = 0;
    hdr.sreq_ptr = sreq;
    hdr.error_bits = coll_attr;
    hdr.flags = MPIDIG_AM_SEND_FLAGS_NONE;
    MPIDIG_AM_SEND_SET_RNDV(hdr.flags, MPIDIG_RNDV_GENERIC_IPC);

    CH4_CALL(am_send_hdr(rank, comm, MPIDIG_SEND, &hdr, sizeof(hdr), vci_src, vci_dst),
             1 /* is_local */ , mpi_errno);
    MPIR_ERR_CHECK(mpi_errno);
    MPIR_REQUEST_SET_INFO(sreq, "MPIDI_IPCI_send_rndv: rank=%d, tag=%d, data_sz=%ld\n", rank, tag,
                          data_sz);

  fn_exit:
    MPIR_FUNC_EXIT;
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

/* Generic receiver side handler for sender-initialized LMT with contig send buffer.
 *
 * The receiver opens the memory handle issued by sender and then performs unpack
 * to its recv buffer. It closes the memory handle after unpack and finally issues
 * LMT_FIN ack to the sender.
 */

MPL_STATIC_INLINE_PREFIX int MPIDI_IPCI_copy_data(MPIDI_IPC_hdr * ipc_hdr, MPIR_Request * req,
                                                  const void *src_buf, MPI_Aint src_data_sz)
{
    int mpi_errno = MPI_SUCCESS;

    if (ipc_hdr->is_contig) {
        MPI_Aint actual_unpack_bytes;
        mpi_errno = MPIR_Typerep_unpack(src_buf, src_data_sz,
                                        MPIDIG_REQUEST(req, buffer), MPIDIG_REQUEST(req, count),
                                        MPIDIG_REQUEST(req, datatype), 0, &actual_unpack_bytes,
                                        MPIR_TYPEREP_FLAG_NONE);
        MPIR_ERR_CHECK(mpi_errno);
        if (actual_unpack_bytes < src_data_sz) {
            MPIR_ERR_SETANDJUMP(mpi_errno, MPI_ERR_TYPE, "**dtypemismatch");
        }
        MPIR_Assert(actual_unpack_bytes == src_data_sz);
    } else {
        void *flattened_type = ipc_hdr + 1;
        MPIR_Datatype *dt = (MPIR_Datatype *) MPIR_Handle_obj_alloc(&MPIR_Datatype_mem);
        MPIR_Assert(dt);
        mpi_errno = MPIR_Typerep_unflatten(dt, flattened_type);
        MPIR_ERR_CHECK(mpi_errno);

        mpi_errno = MPIR_Localcopy(src_buf, ipc_hdr->count, dt->handle,
                                   MPIDIG_REQUEST(req, buffer),
                                   MPIDIG_REQUEST(req, count), MPIDIG_REQUEST(req, datatype));
        MPIR_ERR_CHECK(mpi_errno);
        MPIR_Datatype_free(dt);
    }

  fn_exit:
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

MPL_STATIC_INLINE_PREFIX MPL_gpu_engine_type_t MPIDI_IPCI_choose_engine(int dev1, int dev2)
{
    if (MPIR_CVAR_CH4_IPC_GPU_ENGINE_TYPE == MPIR_CVAR_CH4_IPC_GPU_ENGINE_TYPE_auto) {
        /* Use the high-bandwidth copy engine when either 1) one of the buffers is a host buffer, or
         * 2) the copy is to the same device. Otherwise use the low-latency copy engine. */
        if (dev1 == -1 || dev2 == -1) {
            return MPL_GPU_ENGINE_TYPE_COPY_HIGH_BANDWIDTH;
        }

        if (MPL_gpu_query_is_same_dev(dev1, dev2)) {
            return MPL_GPU_ENGINE_TYPE_COPY_HIGH_BANDWIDTH;
        }

        return MPL_GPU_ENGINE_TYPE_COPY_LOW_LATENCY;
    } else if (MPIR_CVAR_CH4_IPC_GPU_ENGINE_TYPE == MPIR_CVAR_CH4_IPC_GPU_ENGINE_TYPE_compute) {
        return MPL_GPU_ENGINE_TYPE_COMPUTE;
    } else if (MPIR_CVAR_CH4_IPC_GPU_ENGINE_TYPE ==
               MPIR_CVAR_CH4_IPC_GPU_ENGINE_TYPE_copy_high_bandwidth) {
        return MPL_GPU_ENGINE_TYPE_COPY_HIGH_BANDWIDTH;
    } else if (MPIR_CVAR_CH4_IPC_GPU_ENGINE_TYPE ==
               MPIR_CVAR_CH4_IPC_GPU_ENGINE_TYPE_copy_low_latency) {
        return MPL_GPU_ENGINE_TYPE_COPY_LOW_LATENCY;
    } else {
        return MPL_GPU_ENGINE_TYPE_LAST;
    }
}

#endif /* IPC_P2P_H_INCLUDED */
