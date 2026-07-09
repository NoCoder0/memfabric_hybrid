#!/usr/bin/env python
# coding=utf-8
import logging
import os
import torch
import torch.distributed as dist
import torch_npu
import numpy as np
from zbal import zbal_init, zbal_uninit, zbal_set_logger_level

torch_npu.npu.config.allow_internal_format = True
logger = logging.getLogger(__name__)
logging.basicConfig(level=logging.INFO, format='[%(filename)s:%(lineno)d - %(funcName)s()] - %(message)s')

TYPE_MAP = {
    "int": np.int32,
    "int32_t": np.int32,
    "float16_t": np.float16,
    "float": np.float32,
    "bfloat16_t": np.float16,
}
TORCH_TYPE_MAP = {
    "int": torch.int32,
    "int32_t": torch.int32,
    "float16_t": torch.float16,
    "float": torch.float32,
    "bfloat16_t": torch.bfloat16,
}
REPEAT_TIMES = 20
HIDDEN_SIZE = 1024


def init_group(dist_type, world_size, data_op_type=0):
    rank = int(os.environ["RANK"])
    if dist_type == "zbal":
        zbal_set_logger_level(3)
        zbal_init(world_size, int(os.environ["LOCAL_RANK"]), rank, 4 * 1024 * 1024 * 1024, data_op_type=data_op_type)
        group = dist.init_process_group("zbal", rank=rank, world_size=world_size)
    else:
        torch.npu.set_device(int(os.environ["LOCAL_RANK"]))
        group = dist.init_process_group("hccl", rank=rank, world_size=world_size)
    logger.info(f"init {dist_type} group success rank={rank} ws={world_size}")
    return group, rank


def cleanup(group, dist_type):
    dist.destroy_process_group(group)
    if dist_type == "zbal" and not zbal_uninit():
        logger.error("zbal uninit failed.")
