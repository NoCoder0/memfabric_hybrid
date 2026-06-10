#!/bin/bash

CURRENT_DIR=$(cd $(dirname ${BASH_SOURCE:-$0}) && pwd)
echo $CURRENT_DIR

TEST_TYPE=bfloat16_t
export TEST_TYPE=$TEST_TYPE
export CURRENT_DIR=$CURRENT_DIR
rm -rf golden output profiling.*
mkdir -p golden output

RANK_PER_NODE=16
IPs=()
ip_size=${#IPs[@]}

function get_node_idx()
{
    local_ips=`hostname -I | tr ' ' '\n'`
    for ip in $local_ips; do
        for i in "${!IPs[@]}"; do
            if [[ "${IPs[i]}" == "$ip" ]]; then
                echo $i
                return
            fi
        done
    done
    echo "Local-IP-Not-Match"
}

CASE_LIST=${1:-262144}
WORLD_SIZE=${2:-4}
DATA_OP_TYPE=${3:-0}
SUITE_MODE=${4:-perf}
rank_size=${WORLD_SIZE}

# Select Python script and data_gen args based on mode
if [ "$SUITE_MODE" = "smoke" ]; then
    PY_SCRIPT="test_zbal_alltoallv_smoke.py"
    python3 ${CURRENT_DIR}/scripts/data_gen.py $TEST_TYPE $rank_size
    CASE_ARGS=""
else
    PY_SCRIPT="test_zbal_alltoallv_perf.py"
    python3 ${CURRENT_DIR}/scripts/data_gen.py $TEST_TYPE $rank_size --case_list $CASE_LIST
    CASE_ARGS="--case_list $CASE_LIST"
fi

export ENABLE_PROFILING=1
# export HCCL_OP_EXPANSION_MODE="AIV"

nnodes=$(((rank_size + RANK_PER_NODE - 1) / RANK_PER_NODE))
node_rank=$(get_node_idx)
if [[ $nnodes -eq 1 ]]; then
    if [[ ${ZBAL_ENABLE_PERF_TEST} = "1" ]]; then
        echo; echo -e "run hccl..."; torchrun --nnodes ${nnodes} --nproc-per-node $rank_size --master_port 8877 ${CURRENT_DIR}/${PY_SCRIPT} hccl --data_op_type $DATA_OP_TYPE $CASE_ARGS
    fi
    echo; echo -e "run zbal..."; torchrun --nnodes ${nnodes} --nproc-per-node $rank_size --master_port 8877 ${CURRENT_DIR}/${PY_SCRIPT} zbal --data_op_type $DATA_OP_TYPE $CASE_ARGS
else
    if [[ $ip_size -eq $nnodes ]]; then
        if [[ ${ZBAL_ENABLE_PERF_TEST} = "1" ]]; then
            echo; echo -e "run hccl..."; torchrun --nnodes ${nnodes} --nproc-per-node $RANK_PER_NODE --node_rank ${node_rank} --master_addr "${IPs[0]}" --master_port 8877 ${CURRENT_DIR}/${PY_SCRIPT} hccl --data_op_type $DATA_OP_TYPE $CASE_ARGS
        fi
        echo; echo -e "run zbal..."; torchrun --nnodes ${nnodes} --nproc-per-node $RANK_PER_NODE --node_rank ${node_rank} --master_addr "${IPs[0]}" --master_port 8877 ${CURRENT_DIR}/${PY_SCRIPT} zbal --data_op_type $DATA_OP_TYPE $CASE_ARGS
    else
        echo "run ${rank_size} ranks process but IPs size is not match"
    fi
fi
