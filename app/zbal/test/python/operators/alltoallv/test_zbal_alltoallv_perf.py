#!/usr/bin/env python
# coding=utf-8
# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
import logging
import os
import argparse
import torch
import torch.distributed as dist
import torch_npu
import numpy as np
from test_zbal_alltoallv_base import TYPE_MAP, TORCH_TYPE_MAP, REPEAT_TIMES, HIDDEN_SIZE, init_group, cleanup

logger = logging.getLogger(__name__)


def test_alltoallv(dist_type, data_op_type, case_list):
    if not case_list:
        return
    group, global_rank = init_group(dist_type, int(os.environ["WORLD_SIZE"] or 2), data_op_type)
    test_type = os.environ.get("TEST_TYPE", "int")
    current_dir = os.environ.get("CURRENT_DIR", ".")
    data_type = TYPE_MAP.get(test_type, 'int')
    tensor_dtype = TORCH_TYPE_MAP.get(test_type, 'int')
    world_size = int(os.environ["WORLD_SIZE"] or 2)
    enable_profiling = os.environ.get("ENABLE_PROFILING", "0") == "1"

    if enable_profiling:
        prof_cfg = torch_npu.profiler._ExperimentalConfig(
            aic_metrics=torch_npu.profiler.AiCMetrics.PipeUtilization,
            profiler_level=torch_npu.profiler.ProfilerLevel.Level2,
            l2_cache=False,
            data_simplification=False,
        )

    try:
        for case in case_list:
            prof_cnt = 0
            rows = case // HIDDEN_SIZE
            base_split = rows // world_size
            out_dir = f"{current_dir}/output/alltoallv_{case}_{world_size}_{global_rank}/"
            os.makedirs(out_dir, exist_ok=True)

            if dist_type == 'zbal':
                golden = torch.load(f"{out_dir}/output_hccl.bin", weights_only=False).npu()

            if enable_profiling:
                prof = torch_npu.profiler.profile(
                    activities=[torch_npu.profiler.ProfilerActivity.CPU, torch_npu.profiler.ProfilerActivity.NPU],
                    on_trace_ready=torch_npu.profiler.tensorboard_trace_handler(
                        f"{current_dir}/profiling.{dist_type}_{world_size}_{case}/"
                    ),
                    schedule=torch_npu.profiler.schedule(wait=1, warmup=1, active=30, repeat=1, skip_first=4),
                    record_shapes=True,
                    profile_memory=True,
                    with_stack=False,
                    with_flops=False,
                    with_modules=False,
                    experimental_config=prof_cfg,
                )
                torch.npu.synchronize()
                prof.start()

            data = np.fromfile(
                f"{current_dir}/golden/alltoallv_{case}_{world_size}_{global_rank}/input_gm_{global_rank}.bin",
                dtype=data_type,
            )
            tensor_input = torch.from_numpy(data).to(tensor_dtype).npu().view(rows, HIDDEN_SIZE)
            tensor_output = torch.zeros(rows, HIDDEN_SIZE, dtype=tensor_input.dtype, device=tensor_input.device)
            cur_split = [base_split] * world_size

            for j in range(REPEAT_TIMES):
                if enable_profiling and prof_cnt >= 1:
                    prof.step()
                dist.barrier()
                dist.all_to_all_single(tensor_output, tensor_input, cur_split, cur_split)
                prof_cnt += 1
                if dist_type == 'hccl' and j == 0:
                    torch.save(tensor_output.cpu(), f"{out_dir}/output_hccl.bin")
                elif dist_type == 'zbal':
                    if not torch.allclose(tensor_output, golden, rtol=1e-4, atol=1e-8):
                        raise Exception(f"precision error case={case}")

            if enable_profiling:
                torch.npu.synchronize()
                prof.stop()
            logger.info(f"alltoallv {dist_type} case={case} success")
    finally:
        cleanup(group, dist_type)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('dist_type', type=str, choices=["hccl", "zbal"])
    parser.add_argument('--data_op_type', type=int, default=0)
    parser.add_argument('--case_list', type=int, nargs='*', default=[])
    args = parser.parse_args()
    test_alltoallv(args.dist_type, args.data_op_type, args.case_list)
