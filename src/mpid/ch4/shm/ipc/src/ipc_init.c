/*
 * Copyright (C) by Argonne National Laboratory
 *     See COPYRIGHT in top-level directory
 */

#include "mpidimpl.h"
#include "ipc_noinline.h"
#include "ipc_types.h"

int MPIDI_IPC_init_local(void)
{
    int mpi_errno = MPI_SUCCESS;

#ifdef MPL_USE_DBG_LOGGING
    MPIDI_IPCI_DBG_GENERAL = MPL_dbg_class_alloc("SHM_IPC", "shm_ipc");
#endif

    MPIDIG_am_rndv_reg_cb(MPIDIG_RNDV_IPC, &MPIDI_IPC_rndv_cb);
    MPIDIG_am_rndv_reg_cb(MPIDIG_RNDV_GENERIC_IPC, &MPIDI_IPC_do_cts);
    MPIDIG_am_reg_cb(MPIDI_IPC_ACK, NULL, &MPIDI_IPC_ack_target_msg_cb);
    MPIDIG_am_reg_cb(MPIDI_IPC_WRITE, NULL, &MPIDI_IPC_write_target_msg_cb);

#ifdef MPIDI_CH4_SHM_ENABLE_XPMEM
    mpi_errno = MPIDI_XPMEM_init_local();
    MPIR_ERR_CHECK(mpi_errno);
#endif

#ifdef MPIDI_CH4_SHM_ENABLE_GPU
    mpi_errno = MPIDI_GPU_init_local();
    MPIR_ERR_CHECK(mpi_errno);
#endif

    /* extra just to silence potential unused-label warnings */
    MPIR_ERR_CHECK(mpi_errno);

  fn_exit:
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

int MPIDI_IPC_comm_bootstrap(MPIR_Comm * comm)
{
    int mpi_errno = MPI_SUCCESS;

#ifdef MPIDI_CH4_SHM_ENABLE_XPMEM
    mpi_errno = MPIDI_XPMEM_comm_bootstrap(comm);
    MPIR_ERR_CHECK(mpi_errno);
#endif

#ifdef MPIDI_CH4_SHM_ENABLE_CMA
    mpi_errno = MPIDI_CMA_comm_bootstrap(comm);
    MPIR_ERR_CHECK(mpi_errno);
#endif

#ifdef MPIDI_CH4_SHM_ENABLE_GPU
    if (MPIR_CVAR_ENABLE_GPU) {
        mpi_errno = MPIDI_GPU_comm_bootstrap(comm);
        MPIR_ERR_CHECK(mpi_errno);
    }
#endif

    /* extra just to silence potential unused-label warnings */
    MPIR_ERR_CHECK(mpi_errno);

  fn_exit:
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

int MPIDI_IPC_mpi_finalize_hook(void)
{
    int mpi_errno = MPI_SUCCESS;
    MPIR_FUNC_ENTER;

#ifdef MPIDI_CH4_SHM_ENABLE_XPMEM
    mpi_errno = MPIDI_XPMEM_mpi_finalize_hook();
    MPIR_ERR_CHECK(mpi_errno);
#endif

#ifdef MPIDI_CH4_SHM_ENABLE_CMA
    mpi_errno = MPIDI_CMA_mpi_finalize_hook();
    MPIR_ERR_CHECK(mpi_errno);
#endif

#ifdef MPIDI_CH4_SHM_ENABLE_GPU
    mpi_errno = MPIDI_GPU_mpi_finalize_hook();
    MPIR_ERR_CHECK(mpi_errno);
#endif

  fn_exit:
    MPIR_FUNC_EXIT;
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}
