#!/usr/bin/env python
# coding=utf-8
# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
import os
import numpy as np
from scripts.inout_splits import input_shapes
from ml_dtypes import bfloat16

HIDDEN_SIZE = 1024


def gen_random_data_2d(row_num, size, dtype):
    tmp = np.float32 if dtype == bfloat16 else dtype
    return np.random.uniform(low=0.0, high=10.0, size=(row_num, size)).astype(tmp).astype(dtype)


def golden_generate_uniform(case_size, rank_size, data_type, current_dir):
    # Perf mode: uniform splits, all ranks same shape.
    # case_size is total elements per rank = rows * HIDDEN_SIZE.
    h = HIDDEN_SIZE
    rows_per_rank = case_size // h
    for rank_id in range(rank_size):
        golden_dir = f"alltoallv_{case_size}_{rank_size}_{rank_id}/"
        golden_file = f"{current_dir}/golden/{golden_dir}/input_gm_{rank_id}.bin"
        if os.path.isfile(golden_file):
            continue
        os.makedirs(f"{current_dir}/golden/{golden_dir}", exist_ok=True)
        tensor = gen_random_data_2d(rows_per_rank, h, dtype=data_type)
        tensor.tofile(golden_file)


def golden_generate_from_splits(case_index, shapes, data_type, current_dir):
    """Smoke/precision mode: use pre-defined splits."""
    rank_size = len(shapes)
    for rank_id in range(rank_size):
        golden_dir = f"alltoallv_{case_index}_{rank_size}_{rank_id}/"
        golden_file = f"{current_dir}/golden/{golden_dir}/input_gm_{rank_id}.bin"
        if os.path.isfile(golden_file):
            continue
        os.makedirs(f"{current_dir}/golden/{golden_dir}", exist_ok=True)
        shape = shapes[rank_id]
        if len(shape) == 2:
            tensor = gen_random_data_2d(shape[0], shape[1], dtype=data_type)
        else:
            tensor = np.random.uniform(low=0.0, high=10.0, size=(shape[0],)).astype(data_type)
        tensor.tofile(golden_file)


def gen_golden_data():
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument('test_type', type=str)
    parser.add_argument('rank_size', type=int)
    parser.add_argument('--case_list', type=int, nargs='*', default=[])
    args = parser.parse_args()

    type_map = {
        "int": np.int32,
        "int32_t": np.int32,
        "float16_t": np.float16,
        "float": np.float32,
        "bfloat16_t": bfloat16,
    }
    current_dir = os.getenv("CURRENT_DIR", ".")
    data_type = type_map.get(args.test_type, 'int')
    rank_size = args.rank_size

    if args.case_list:
        # Perf mode: uniform splits
        for case in args.case_list:
            golden_generate_uniform(case, rank_size, data_type, current_dir)
    else:
        # Smoke/precision mode: from inout_splits
        for i in range(len(input_shapes)):
            if len(input_shapes[i]) == rank_size:
                golden_generate_from_splits(i, input_shapes[i], data_type, current_dir)


if __name__ == '__main__':
    gen_golden_data()
