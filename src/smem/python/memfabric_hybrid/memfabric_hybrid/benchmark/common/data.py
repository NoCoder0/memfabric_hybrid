# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""Known payload patterns and byte comparison for data verification."""

import torch

_RANDOM_MULTIPLIER = 23
_RANDOM_INCREMENT = 17
_NEGATIVE_RATIO_DIVISOR = 3


def generate_data(tensor, rank_id):
    arr = tensor.flatten()
    mod = 2**15 - 1
    base = initial = rank_id % mod
    values = []
    # The recurrence is periodic. Build one complete signed period, then let
    # Torch repeat it instead of assigning millions of tensor scalars in Python.
    for i in range(min(len(arr), mod * _NEGATIVE_RATIO_DIVISOR)):
        base = (base * _RANDOM_MULTIPLIER + _RANDOM_INCREMENT) % mod
        values.append(-base if (i + rank_id) % _NEGATIVE_RATIO_DIVISOR == 0 else base)
        if base == initial and (i + 1) % _NEGATIVE_RATIO_DIVISOR == 0:
            break
    if values:
        pattern = torch.tensor(values, dtype=torch.int32)
        arr.copy_(pattern.repeat((len(arr) + len(values) - 1) // len(values))[: len(arr)])


def wrong_bytes(expected, actual):
    expected_bytes, actual_bytes = expected.view(torch.uint8), actual.view(torch.uint8)
    if torch.equal(expected_bytes, actual_bytes):
        return 0
    return int((expected_bytes != actual_bytes).sum().item())
