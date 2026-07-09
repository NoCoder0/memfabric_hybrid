#!/usr/bin/env python
# coding=utf-8
# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
import os
import argparse
import torch
import torch.distributed as dist
import numpy as np
from scripts.inout_splits import input_shapes, output_shapes, input_splits, output_splits
from test_zbal_alltoallv_base import TYPE_MAP, TORCH_TYPE_MAP, REPEAT_TIMES, init_group, cleanup, logger


def get_cur_cases(expect_world_size):
    cur_in, cur_out, cur_is, cur_os, idx = [], [], [], [], []
    for i, s in enumerate(input_shapes):
        if len(s) == expect_world_size:
            cur_in.append(s)
            cur_out.append(output_shapes[i])
            cur_is.append(input_splits[i])
            cur_os.append(output_splits[i])
            idx.append(i)
    return cur_in, cur_out, cur_is, cur_os, idx


def _load_tensor(golden_file, cur_shape, data_type, tensor_dtype):
    data = np.fromfile(golden_file, dtype=data_type)
    t = torch.from_numpy(data).to(tensor_dtype).npu()
    return t.view(*cur_shape) if len(cur_shape) == 2 else t.view(cur_shape[0])


def _assemble_golden(
    case_id, world_size, rank, cur_input_splits, cur_output_splits, output_shape, data_type, tensor_dtype, current_dir
):
    parts = []
    for sender in range(world_size):
        if cur_output_splits[rank][sender] == 0:
            continue
        f = f"{current_dir}/golden/alltoallv_{case_id}_{world_size}_{sender}/input_gm_{sender}.bin"
        t = torch.from_numpy(np.fromfile(f, dtype=data_type)).to(tensor_dtype)
        ss = cur_input_splits[sender]
        elem = t.numel() // sum(ss) if sum(ss) > 0 else 0
        off = sum(ss[:rank]) * elem
        parts.append(t[off : off + cur_output_splits[rank][sender] * elem])
    if not parts:
        return (
            torch.zeros(0, output_shape[1], dtype=tensor_dtype).npu()
            if len(output_shape) == 2
            else torch.tensor([], dtype=tensor_dtype).npu()
        )
    r = torch.cat(parts, dim=0).npu()
    return r.view(output_shape) if len(output_shape) == 2 else r


def test_alltoallv(dist_type, data_op_type):
    world_size = int(os.environ["WORLD_SIZE"] or 2)
    test_type = os.environ.get("TEST_TYPE", "int")
    current_dir = os.environ.get("CURRENT_DIR", ".")
    data_type = TYPE_MAP.get(test_type, 'int')
    tensor_dtype = TORCH_TYPE_MAP.get(test_type, 'int')
    shapes_in, shapes_out, splits_in, splits_out, case_index = get_cur_cases(world_size)
    if not shapes_in:
        return

    group, global_rank = init_group(dist_type, world_size, data_op_type)

    try:
        for i in range(len(case_index)):
            case_id = case_index[i]
            out_dir = f"{current_dir}/output/alltoallv_{case_id}_{world_size}_{global_rank}/"
            os.makedirs(out_dir, exist_ok=True)

            golden = None
            if dist_type == 'zbal':
                golden = _assemble_golden(
                    case_id,
                    world_size,
                    global_rank,
                    splits_in[i],
                    splits_out[i],
                    shapes_out[i][global_rank],
                    data_type,
                    tensor_dtype,
                    current_dir,
                )

            in_shape = shapes_in[i][global_rank]
            gd = f"alltoallv_{case_id}_{world_size}_{global_rank}"
            tensor_input = _load_tensor(
                f"{current_dir}/golden/{gd}/input_gm_{global_rank}.bin", in_shape, data_type, tensor_dtype
            )

            for j in range(REPEAT_TIMES):
                try:
                    tensor_output = torch.zeros(
                        shapes_out[i][global_rank], dtype=tensor_input.dtype, device=tensor_input.device
                    )
                    dist.all_to_all_single(
                        tensor_output, tensor_input, splits_out[i][global_rank], splits_in[i][global_rank]
                    )
                    if dist_type == 'hccl' and j == 0:
                        torch.save(tensor_output.cpu(), f"{out_dir}/output_{dist_type}.bin")
                    elif dist_type == 'zbal':
                        if not torch.allclose(tensor_output, golden, rtol=1e-4, atol=1e-8):
                            raise ValueError("precision error")
                except Exception:
                    logger.exception(f"{dist_type} case {case_id} round {j} failed")
                    raise

        logger.info(f"alltoallv {dist_type} {len(case_index)} cases x {REPEAT_TIMES} success")
    finally:
        cleanup(group, dist_type)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('dist_type', type=str, choices=["hccl", "zbal"])
    parser.add_argument('--data_op_type', type=int, default=0)
    args = parser.parse_args()
    test_alltoallv(args.dist_type, args.data_op_type)
