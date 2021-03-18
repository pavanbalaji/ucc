/**
 * Copyright (C) Mellanox Technologies Ltd. 2021.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#include "tl_nccl_coll.h"
#include "utils/ucc_math.h"
#include "utils/ucc_coll_utils.h"

#define ncclOpUnsupported (ncclNumOps + 1)
#define ncclDataTypeUnsupported (ncclNumTypes + 1)

ncclDataType_t ucc_to_nccl_dtype[] = {
    [UCC_DT_INT8]        = ncclInt8,
    [UCC_DT_INT16]       = ncclDataTypeUnsupported,
    [UCC_DT_INT32]       = ncclInt32,
    [UCC_DT_INT64]       = ncclInt64,
    [UCC_DT_INT128]      = ncclDataTypeUnsupported,
    [UCC_DT_UINT8]       = ncclUint8,
    [UCC_DT_UINT16]      = ncclDataTypeUnsupported,
    [UCC_DT_UINT32]      = ncclUint32,
    [UCC_DT_UINT64]      = ncclUint64,
    [UCC_DT_UINT128]     = ncclDataTypeUnsupported,
    [UCC_DT_FLOAT16]     = ncclFloat16,
    [UCC_DT_FLOAT32]     = ncclFloat32,
    [UCC_DT_FLOAT64]     = ncclFloat64,
    [UCC_DT_USERDEFINED] = ncclDataTypeUnsupported,
    [UCC_DT_OPAQUE]      = ncclDataTypeUnsupported,
};

ncclRedOp_t ucc_to_nccl_reduce_op[] = {
    [UCC_OP_USERDEFINED] = ncclOpUnsupported,
    [UCC_OP_SUM]         = ncclSum,
    [UCC_OP_PROD]        = ncclProd,
    [UCC_OP_MAX]         = ncclMax,
    [UCC_OP_MIN]         = ncclMin,
    [UCC_OP_LAND]        = ncclOpUnsupported,
    [UCC_OP_LOR]         = ncclOpUnsupported,
    [UCC_OP_LXOR]        = ncclOpUnsupported,
    [UCC_OP_BAND]        = ncclOpUnsupported,
    [UCC_OP_BOR]         = ncclOpUnsupported,
    [UCC_OP_BXOR]        = ncclOpUnsupported,
    [UCC_OP_MAXLOC]      = ncclOpUnsupported,
    [UCC_OP_MINLOC]      = ncclOpUnsupported,
};

ucc_status_t ucc_nccl_collective_progress(ucc_coll_task_t *coll_task)
{
    ucc_tl_nccl_task_t *task = ucc_derived_of(coll_task, ucc_tl_nccl_task_t);
    cudaError_t cuda_st;

    cuda_st = cudaEventQuery(task->completed);
    switch (cuda_st) {
    case cudaSuccess:
        coll_task->super.status = UCC_OK;
        return UCC_OK;
    case cudaErrorNotReady:
        return UCC_INPROGRESS;
    default:
        return UCC_ERR_NO_MESSAGE;
    }
}

ucc_status_t ucc_nccl_alltoall_start(ucc_coll_task_t *coll_task)
{
    ucc_tl_nccl_task_t *task   = ucc_derived_of(coll_task, ucc_tl_nccl_task_t);
    ucc_tl_nccl_team_t *team   = task->team;
    cudaStream_t        stream = team->stream;
    ucc_rank_t          gsize  = team->size;
    ucc_status_t        status = UCC_OK;
    ptrdiff_t           sbuf   = (ptrdiff_t)task->args.src.info.buffer;
    ptrdiff_t           rbuf   = (ptrdiff_t)task->args.dst.info.buffer;
    size_t data_size;
    ucc_rank_t peer;

    task->super.super.status = UCC_INPROGRESS;
    data_size = (size_t)task->args.src.info.count *
                ucc_dt_size(task->args.src.info.datatype);
    if (data_size == 0) {
        task->super.super.status = UCC_OK;
        return UCC_OK;
    }
    NCCLCHECK_GOTO(ncclGroupStart(), exit_coll, status, UCC_TL_TEAM_LIB(team));
    for (peer = 0; peer < gsize; peer++) {
        NCCLCHECK_GOTO(ncclSend((void *)(sbuf + peer * data_size), data_size,
                                ncclChar, peer, team->nccl_comm, stream),
                       exit_coll, status, UCC_TL_TEAM_LIB(team));
        NCCLCHECK_GOTO(ncclRecv((void *)(rbuf + peer * data_size), data_size,
                                ncclChar, peer, team->nccl_comm, stream),
                       exit_coll, status, UCC_TL_TEAM_LIB(team));
    }
    NCCLCHECK_GOTO(ncclGroupEnd(), exit_coll, status, UCC_TL_TEAM_LIB(team));

exit_coll:
    if (status == UCC_OK) {
        ucc_progress_enqueue(UCC_TL_CORE_CTX(team)->pq, &task->super);
    } else if (status < 0) {
        task->super.super.status = status;
    }
    return status;
}

ucc_status_t ucc_tl_nccl_alltoall_init(ucc_tl_nccl_task_t *task)
{
    if (UCC_IS_INPLACE(task->args)) {
        tl_error(UCC_TL_TEAM_LIB(task->team),
                 "inplace alltoallv is not supported");
        return UCC_ERR_NOT_SUPPORTED;
    }
    if ((task->args.src.info.datatype == UCC_DT_USERDEFINED) ||
        (task->args.dst.info.datatype == UCC_DT_USERDEFINED)) {
        tl_error(UCC_TL_TEAM_LIB(task->team),
                 "user defined datatype is not supported");
        return UCC_ERR_NOT_SUPPORTED;
    }
    task->super.post     = ucc_nccl_alltoall_start;
    task->super.progress = ucc_nccl_collective_progress;
    return UCC_OK;
}

ucc_status_t ucc_nccl_alltoallv_start(ucc_coll_task_t *coll_task)
{
    ucc_tl_nccl_task_t *task   = ucc_derived_of(coll_task, ucc_tl_nccl_task_t);
    ucc_tl_nccl_team_t *team   = task->team;
    cudaStream_t        stream = team->stream;
    ucc_status_t        status = UCC_OK;
    ptrdiff_t           sbuf   = (ptrdiff_t)task->args.src.info.buffer;
    ptrdiff_t           rbuf   = (ptrdiff_t)task->args.dst.info.buffer;
    size_t sdt_size, rdt_size, count, displ;
    ucc_rank_t peer;

    task->super.super.status = UCC_INPROGRESS;
    sdt_size = ucc_dt_size(task->args.src.info_v.datatype);
    rdt_size = ucc_dt_size(task->args.dst.info_v.datatype);
    NCCLCHECK_GOTO(ncclGroupStart(), exit_coll, status, UCC_TL_TEAM_LIB(team));
    for (peer = 0; peer < team->size; peer++) {
        count = ucc_coll_args_get_count(&task->args,
                                        task->args.src.info_v.counts, peer);
        if (count != 0) {
            displ = ucc_coll_args_get_displacement(&task->args,
                        task->args.src.info_v.displacements, peer);
            NCCLCHECK_GOTO(ncclSend((void *)(sbuf + displ * sdt_size),
                                    count * sdt_size, ncclChar, peer,
                                    team->nccl_comm, stream),
                        exit_coll, status, UCC_TL_TEAM_LIB(team));
        }
        count = ucc_coll_args_get_count(&task->args,
                                        task->args.dst.info_v.counts, peer);
        if (count != 0) {
            displ = ucc_coll_args_get_displacement(&task->args,
                        task->args.dst.info_v.displacements, peer);
            NCCLCHECK_GOTO(ncclRecv((void *)(rbuf + displ * rdt_size),
                                    count * rdt_size, ncclChar, peer,
                                    team->nccl_comm, stream),
                        exit_coll, status, UCC_TL_TEAM_LIB(team));
        }
    }
    NCCLCHECK_GOTO(ncclGroupEnd(), exit_coll, status, UCC_TL_TEAM_LIB(team));

exit_coll:
    if (status == UCC_OK) {
        ucc_progress_enqueue(UCC_TL_CORE_CTX(team)->pq, &task->super);
    } else if (status < 0) {
        task->super.super.status = status;
    }
    return status;
}

ucc_status_t ucc_tl_nccl_alltoallv_init(ucc_tl_nccl_task_t *task)
{
    if (UCC_IS_INPLACE(task->args)) {
        tl_error(UCC_TL_TEAM_LIB(task->team),
                 "inplace alltoall is not supported");
        return UCC_ERR_NOT_SUPPORTED;
    }
    if ((task->args.src.info_v.datatype == UCC_DT_USERDEFINED) ||
        (task->args.dst.info_v.datatype == UCC_DT_USERDEFINED)) {
        tl_error(UCC_TL_TEAM_LIB(task->team),
                 "user defined datatype is not supported");
        return UCC_ERR_NOT_SUPPORTED;
    }
    task->super.post     = ucc_nccl_alltoallv_start;
    task->super.progress = ucc_nccl_collective_progress;
    return UCC_OK;
}
