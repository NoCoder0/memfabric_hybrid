# vllm ascend + mooncake对接memfabric_hybrid部署指南

## 环境依赖

**软件要求**

- CANN >= 8.5.0
- memfabric-hybrid: release/1.1 分支
- vllM-Ascend: main 分支
- Mooncake: v0.3.9 tag

**硬件要求**

- A3

## 安装memfabric-hybrid

1. 安装memfabric-hybrid

    `pip install memfabric-hybrid`

2. 下载memfabric-hybrid 适配Mooncake的patch文件

    `https://gitcode.com/Ascend/memfabric_hybrid/blob/release/1.1/patches/mooncake/tag_v0.3.9.patch`

## 安装Mooncake

1. Mooncake 是 Moonshot AI 提供的领先 LLM 服务 Kimi 的推理平台。首先，我们需要获取 Mooncake 项目。参考以下命令：

    `git clone -b v0.3.9 --depth 1 https://github.com/kvcache-ai/Mooncake.git`

2. 拉取Mooncake 依赖

    ```bash
    cd Mooncake
    git submodule update --init --recursive
    ```

3. 应用memfabric_hybrid 适配Mooncake的patch

    `git apply  ../tag_v0.3.9.patch`

4. 编译与安装

    ```bash
    mkdir -p build
    cd build
    cmake .. -DUSE_UB=ON
    make -j
    make install
    ```

## 启动Mooncake Master

```bash
# 在任意一个节点拉起一个Mooncake Master 即可
export LD_LIBRARY_PATH=/usr/local/Ascend/ascend-toolkit/latest/python/site-packages/mooncake:$LD_LIBRARY_PATH
mooncake_master --eviction_high_watermark_ratio 0.9 --eviction_ratio 0.05 --port 50021
```

## 配置mooncake.json

在所有节点都需要配置这个配置文件，其中local_hostname建议设置为各个节点的IP，master_server_address配置为Mooncake Master拉起节点的IP

```ini
{
    "local_hostname": "xx.xx.xx.xx",
    "metadata_server": "P2PHANDSHAKE",
    "protocol": "ub",
    "device_name": "",
    "use_ascend_direct": true,
    "master_server_address": "141.61.41.87:50021",
    "global_segment_size": 17179869184
}
```

## PD 分离场景(2机 1P 1D场景，dp=1)

### prefill节点脚本如下

run_dp_template.sh

```shell
nic_name="enp194s0f0" # change to your own nic name
local_ip="141.61.39.233" # change to your own ip

MOONCAKE_CONFIG_PATH="/home/lz/MY_test/mooncake.json"
MF_DRAM_SIZE=8589934592
MF_STORE_URL="tcp://141.61.39.233:7890"
MF_LOG_LEVEL=4

export MF_LOG_LEVEL=$MF_LOG_LEVEL
export MF_DRAM_SIZE=$MF_DRAM_SIZE
export MF_OP_TYPE=device_sdma
export MF_STORE_URL="$MF_STORE_URL"
export MF_SMEM_TRANS_ROLE=Prefill
export MF_PD_OP_TYPE=device_sdma
export MOONCAKE_CONFIG_PATH="$MOONCAKE_CONFIG_PATH"
export VLLM_USE_V1=1

export HCCL_OP_EXPANSION_MODE="AIV"

export HCCL_IF_IP=$local_ip
export GLOO_SOCKET_IFNAME=$nic_name
export TP_SOCKET_IFNAME=$nic_name
export HCCL_SOCKET_IFNAME=$nic_name

export OMP_PROC_BIND=false
export OMP_NUM_THREADS=1
export PYTORCH_NPU_ALLOC_CONF=expandable_segments:True
export HCCL_BUFFSIZE=256

export ASCEND_AGGREGATE_ENABLE=1
export ASCEND_TRANSPORT_PRINT=1
export ACL_OP_INIT_MODE=1
export ASCEND_A3_ENABLE=1
export VLLM_NIXL_ABORT_REQUEST_TIMEOUT=300000

export ASCEND_RT_VISIBLE_DEVICES=$1

# export VLLM_ASCEND_ENABLE_FLASHCOMM1=1

export VLLM_ASCEND_ENABLE_FUSED_MC2=1
export VLLM_ASCEND_ENABLE_MLAPO=1
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/lib

vllm serve /home/weights/GLM-5-w8a8/ \
    --host 0.0.0.0 \
    --port $2 \
    --tensor-parallel-size $7 \
    --enable-expert-parallel \
    --profiler-config \
    '{"profiler": "torch",
    "torch_profiler_dir": "./vllm_profile",
    "torch_profiler_with_stack": false}' \
    --seed 1024 \
    --served-model-name glm-5 \
    --max-model-len 35000 \
    --additional-config '{"enable_npugraph_ex": true, "fuse_muls_add":true,"multistream_overlap_shared_expert":true,"recompute_scheduler_enable" : true}' \
    --max-num-batched-tokens 4096 \
    --trust-remote-code \
    --max-num-seqs 64 \
    --gpu-memory-utilization 0.95 \
    --enforce-eager \
    --enable-auto-tool-choice \
    --tool-call-parser glm47 \
    --reasoning-parser glm45 \
    --kv-transfer-config \
    '{
        "kv_connector": "MultiConnector",
        "kv_role": "kv_producer",
        "kv_connector_extra_config": {
        "use_layerwise": false,
        "connectors": [
        {
        "kv_connector": "MooncakeConnectorV1",
        "kv_role": "kv_producer",
        "kv_port": "30010",
        "kv_connector_extra_config": {
            "use_ascend_direct": true,
            "prefill": {
            "dp_size": 1,
            "tp_size": 16
            },
            "decode": {
            "dp_size": 1,
            "tp_size": 16
            }
        }
        },
        {
        "kv_connector": "MooncakeConnectorStoreV1",
        "kv_role": "kv_producer",
        "kv_connector_extra_config": {
            "mooncake_rpc_port": "0"
        }
        }
    ]
    }
}'
```

### decode节点如下

run_dp_template.sh

```shell
local_ip="141.61.39.235" # change to your own ip
MOONCAKE_CONFIG_PATH="/home/lz/MY_test/mooncake.json"
MF_DRAM_SIZE=8589934592
MF_STORE_URL="tcp://141.61.39.235:7890"
MF_LOG_LEVEL=4

export MF_LOG_LEVEL=$MF_LOG_LEVEL
export MF_DRAM_SIZE=$MF_DRAM_SIZE
export MF_OP_TYPE=device_sdma
export MF_STORE_URL="$MF_STORE_URL"
export MF_SMEM_TRANS_ROLE=Decode
export MF_PD_OP_TYPE=device_sdma
export MOONCAKE_CONFIG_PATH="$MOONCAKE_CONFIG_PATH"
export VLLM_USE_V1=1

export HCCL_OP_EXPANSION_MODE="AIV"

export HCCL_IF_IP=$local_ip
export GLOO_SOCKET_IFNAME=$nic_name
export TP_SOCKET_IFNAME=$nic_name
export HCCL_SOCKET_IFNAME=$nic_name

#Mooncake
export OMP_PROC_BIND=false
export OMP_NUM_THREADS=1

export PYTORCH_NPU_ALLOC_CONF=expandable_segments:True
export HCCL_BUFFSIZE=256


export ASCEND_AGGREGATE_ENABLE=1
export ASCEND_TRANSPORT_PRINT=1
export ACL_OP_INIT_MODE=1
export ASCEND_A3_ENABLE=1
export VLLM_NIXL_ABORT_REQUEST_TIMEOUT=300000

export TASK_QUEUE_ENABLE=1

export ASCEND_RT_VISIBLE_DEVICES=$1

export VLLM_ASCEND_ENABLE_FUSED_MC2=1
export VLLM_ASCEND_ENABLE_MLAPO=1
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/lib

vllm serve /home/weights/GLM-5-w8a8/ \
    --host 0.0.0.0 \
    --port $2 \
    --data-parallel-size $3 \
    --tensor-parallel-size $7 \
    --enable-expert-parallel \
    --profiler-config \
    '{"profiler": "torch",
    "torch_profiler_dir": "./vllm_profile",
    "torch_profiler_with_stack": false}' \
    --seed 1024 \
    --served-model-name glm-5 \
    --max-model-len 30000 \
    --max-num-batched-tokens 32 \
    --compilation-config '{"cudagraph_mode":"FULL_DECODE_ONLY", "cudagraph_capture_sizes":[4, 8, 12, 16,20,24,28, 32]}' \
    --additional-config '{"enable_npugraph_ex": true, "fuse_muls_add":true,"multistream_overlap_shared_expert":true,"recompute_scheduler_enable" : true}' \
    --trust-remote-code \
    --max-num-seqs 8 \
    --gpu-memory-utilization 0.92 \
    --async-scheduling \
    --enable-auto-tool-choice \
    --tool-call-parser glm47 \
    --reasoning-parser glm45 \
    --kv-transfer-config \
    '{
    "kv_connector": "MultiConnector",
    "kv_role": "kv_producer",
    "kv_connector_extra_config": {
    "use_layerwise": false,
    "connectors": [
    {
        "kv_connector": "MooncakeConnectorV1",
        "kv_role": "kv_consumer",
        "kv_port": "30020",
        "kv_connector_extra_config": {
        "use_ascend_direct": true,
        "prefill": {
            "dp_size": 1,
            "tp_size": 16
        },
        "decode": {
            "dp_size": 1,
            "tp_size": 16
        }
        }
    },
    {
        "kv_connector": "MooncakeConnectorStoreV1",
        "kv_role": "kv_producer",
        "kv_connector_extra_config": {
        "mooncake_rpc_port": "0"
        }
    }
    ]
}
}'
```

### 获取各个节点启动prefill 或 decode需要的脚本

```ini
# 下载此文件和vllm 启动脚本同一目录
https://github.com/vllm-project/vllm-ascend/blob/main/examples/external_online_dp/launch_online_dp.py
```

### 各节点启动命令如下

```bash
# # change ip to your own
python launch_online_dp.py --dp-size 1 --tp-size 16 --dp-size-local 1 --dp-rank-start 0 --dp-address 141.61.39.233 --dp-rpc-port 10521 --vllm-start-port 6700

# # change ip to your own
python launch_online_dp.py --dp-size 1 --tp-size 16 --dp-size-local 1 --dp-rank-start 0 --dp-address 141.61.39.235 --dp-rpc-port 10523 --vllm-start-port 6721
```

### 获取proxy代理需要的脚本

```ini
# 下载以下脚本在任意节点拉起即可
https://github.com/vllm-project/vllm-ascend/blob/main/examples/disaggregated_prefill_v1/load_balance_proxy_server_example.py
```

## PD 分离场景(4机 1P 1D场景，dp=2)

### prefill 节点脚本如下

run_dp_template.sh

```shell
nic_name="enp194s0f0" # change to your own nic name
local_ip="141.61.39.233" # change to your own ip

MOONCAKE_CONFIG_PATH="/home/lz/MY_test/mooncake.json"
MF_DRAM_SIZE=8589934592
MF_STORE_URL="tcp://141.61.39.233:7890"
MF_LOG_LEVEL=4

export MF_LOG_LEVEL=$MF_LOG_LEVEL
export MF_DRAM_SIZE=$MF_DRAM_SIZE
export MF_OP_TYPE=device_sdma
export MF_STORE_URL="$MF_STORE_URL"
export MF_SMEM_TRANS_ROLE=Prefill
export MF_PD_OP_TYPE=device_sdma
export MOONCAKE_CONFIG_PATH="$MOONCAKE_CONFIG_PATH"
export VLLM_USE_V1=1

export HCCL_OP_EXPANSION_MODE="AIV"

export HCCL_IF_IP=$local_ip
export GLOO_SOCKET_IFNAME=$nic_name
export TP_SOCKET_IFNAME=$nic_name
export HCCL_SOCKET_IFNAME=$nic_name

export OMP_PROC_BIND=false
export OMP_NUM_THREADS=1
export PYTORCH_NPU_ALLOC_CONF=expandable_segments:True
export HCCL_BUFFSIZE=256

export ASCEND_AGGREGATE_ENABLE=1
export ASCEND_TRANSPORT_PRINT=1
export ACL_OP_INIT_MODE=1
export ASCEND_A3_ENABLE=1
export VLLM_NIXL_ABORT_REQUEST_TIMEOUT=300000

export ASCEND_RT_VISIBLE_DEVICES=$1

# export VLLM_ASCEND_ENABLE_FLASHCOMM1=1

export VLLM_ASCEND_ENABLE_FUSED_MC2=1
export VLLM_ASCEND_ENABLE_MLAPO=1
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/lib

vllm serve /home/weights/GLM-5-w8a8/ \
    --host 0.0.0.0 \
    --port $2 \
    --data-parallel-size $3 \
    --data-parallel-rank $4 \
    --data-parallel-address $5 \
    --data-parallel-rpc-port $6 \
    --tensor-parallel-size $7 \
    --enable-expert-parallel \
    --profiler-config \
    '{"profiler": "torch",
    "torch_profiler_dir": "./vllm_profile",
    "torch_profiler_with_stack": false}' \
    --seed 1024 \
    --served-model-name glm-5 \
    --max-model-len 35000 \
    --additional-config '{"enable_npugraph_ex": true, "fuse_muls_add":true,"multistream_overlap_shared_expert":true,"recompute_scheduler_enable" : true}' \
    --max-num-batched-tokens 4096 \
    --trust-remote-code \
    --max-num-seqs 64 \
    --gpu-memory-utilization 0.95 \
    --enforce-eager \
    --enable-auto-tool-choice \
    --tool-call-parser glm47 \
    --reasoning-parser glm45 \
    --kv-transfer-config \
    '{
        "kv_connector": "MultiConnector",
        "kv_role": "kv_producer",
        "kv_connector_extra_config": {
        "use_layerwise": false,
        "connectors": [
        {
        "kv_connector": "MooncakeConnectorV1",
        "kv_role": "kv_producer",
        "kv_port": "30010",
        "kv_connector_extra_config": {
            "use_ascend_direct": true,
            "prefill": {
            "dp_size": 2,
            "tp_size": 16
            },
            "decode": {
            "dp_size": 2,
            "tp_size": 16
            }
        }
        },
        {
        "kv_connector": "MooncakeConnectorStoreV1",
        "kv_role": "kv_producer",
        "kv_connector_extra_config": {
            "mooncake_rpc_port": "0"
        }
        }
    ]
    }
}'
```

### decode节点脚本如下

run_dp_template.sh

```shell
local_ip="141.61.39.235" # change to your own ip
MOONCAKE_CONFIG_PATH="/home/lz/MY_test/mooncake.json"
MF_DRAM_SIZE=8589934592
MF_STORE_URL="tcp://141.61.39.235:7890"
MF_LOG_LEVEL=4

export MF_LOG_LEVEL=$MF_LOG_LEVEL
export MF_DRAM_SIZE=$MF_DRAM_SIZE
export MF_OP_TYPE=device_sdma
export MF_STORE_URL="$MF_STORE_URL"
export MF_SMEM_TRANS_ROLE=Decode
export MF_PD_OP_TYPE=device_sdma
export MOONCAKE_CONFIG_PATH="$MOONCAKE_CONFIG_PATH"
export VLLM_USE_V1=1

export HCCL_OP_EXPANSION_MODE="AIV"

export HCCL_IF_IP=$local_ip
export GLOO_SOCKET_IFNAME=$nic_name
export TP_SOCKET_IFNAME=$nic_name
export HCCL_SOCKET_IFNAME=$nic_name

#Mooncake
export OMP_PROC_BIND=false
export OMP_NUM_THREADS=1

export PYTORCH_NPU_ALLOC_CONF=expandable_segments:True
export HCCL_BUFFSIZE=256


export ASCEND_AGGREGATE_ENABLE=1
export ASCEND_TRANSPORT_PRINT=1
export ACL_OP_INIT_MODE=1
export ASCEND_A3_ENABLE=1
export VLLM_NIXL_ABORT_REQUEST_TIMEOUT=300000

export TASK_QUEUE_ENABLE=1

export ASCEND_RT_VISIBLE_DEVICES=$1

export VLLM_ASCEND_ENABLE_FUSED_MC2=1
export VLLM_ASCEND_ENABLE_MLAPO=1
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/lib

vllm serve /home/weights/GLM-5-w8a8/ \
    --host 0.0.0.0 \
    --port $2 \
    --data-parallel-size $3 \
    --data-parallel-rank $4 \
    --data-parallel-address $5 \
    --data-parallel-rpc-port $6 \
    --data-parallel-size $3 \
    --tensor-parallel-size $7 \
    --enable-expert-parallel \
    --profiler-config \
    '{"profiler": "torch",
    "torch_profiler_dir": "./vllm_profile",
    "torch_profiler_with_stack": false}' \
    --seed 1024 \
    --served-model-name glm-5 \
    --max-model-len 30000 \
    --max-num-batched-tokens 32 \
    --compilation-config '{"cudagraph_mode":"FULL_DECODE_ONLY", "cudagraph_capture_sizes":[4, 8, 12, 16,20,24,28, 32]}' \
    --additional-config '{"enable_npugraph_ex": true, "fuse_muls_add":true,"multistream_overlap_shared_expert":true,"recompute_scheduler_enable" : true}' \
    --trust-remote-code \
    --max-num-seqs 8 \
    --gpu-memory-utilization 0.92 \
    --async-scheduling \
    --enable-auto-tool-choice \
    --tool-call-parser glm47 \
    --reasoning-parser glm45 \
    --kv-transfer-config \
    '{
    "kv_connector": "MultiConnector",
    "kv_role": "kv_producer",
    "kv_connector_extra_config": {
    "use_layerwise": false,
    "connectors": [
    {
        "kv_connector": "MooncakeConnectorV1",
        "kv_role": "kv_consumer",
        "kv_port": "30020",
        "kv_connector_extra_config": {
        "use_ascend_direct": true,
        "prefill": {
            "dp_size": 2,
            "tp_size": 16
        },
        "decode": {
            "dp_size": 2,
            "tp_size": 16
        }
        }
    },
    {
        "kv_connector": "MooncakeConnectorStoreV1",
        "kv_role": "kv_producer",
        "kv_connector_extra_config": {
        "mooncake_rpc_port": "0"
        }
    }
    ]
}
}'
```

### 获取各个节点启动prefill 或 decode需要的脚本

```ini
# 下载此文件和vllm 启动脚本同一目录
https://github.com/vllm-project/vllm-ascend/blob/main/examples/external_online_dp/launch_online_dp.py
```

### 各节点启动命令如下

```bash
# # change ip to your own
python launch_online_dp.py --dp-size 2 --tp-size 16 --dp-size-local 1 --dp-rank-start 0 --dp-address 10.247.162.10 --dp-rpc-port 10521 --vllm-start-port 6700

# # change ip to your own
python launch_online_dp.py --dp-size 2 --tp-size 16 --dp-size-local 1 --dp-rank-start 1 --dp-address 10.247.162.10 --dp-rpc-port 10521 --vllm-start-port 6700

# # change ip to your own
python launch_online_dp.py --dp-size 2 --tp-size 16 --dp-size-local 1 --dp-rank-start 0 --dp-address 10.247.162.12 --dp-rpc-port 10523 --vllm-start-port 6721

# # change ip to your own
python launch_online_dp.py --dp-size 2 --tp-size 16 --dp-size-local 1 --dp-rank-start 1 --dp-address 10.247.162.12 --dp-rpc-port 10523 --vllm-start-port 6721
```
