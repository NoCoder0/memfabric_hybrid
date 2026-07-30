# H2H URMA — Two Kunpeng Servers

Verifies host-to-host HCOMM URMA data path with `HostUrmaTransportManager`.

## Build

Both Kunpeng nodes:

```bash
bash script/build_and_pack_run.sh \
    --xpu_type NONE \
    --build_hcom ON \
    --build_hcom_rdma OFF
```

## Run

```bash
# Node A (rank 0)
python3 03_multi_node_host_urma_dram.py \
    --rank 0 \
    --head-ip 192.168.10.10 \
    --local-urma-ip 192.168.20.10

# Node B (rank 1)
python3 03_multi_node_host_urma_dram.py \
    --rank 1 \
    --head-ip 192.168.10.10 \
    --local-urma-ip 192.168.30.10
```

## What It Tests

- H2H channel creation (`COMM_ENGINE_CPU`)
- DDR registration/export/import with GVA equality
- `ReadRemote` (rank 1 reads rank 0)
- `WriteRemote` (rank 1 writes rank 0)
- Batch fallback (`ReadRemoteBatchAsync` via `copy_data_batch`)
- Bidirectional verification
- 100× 4 MiB throughput loop
- Clean CloseDevice (MR/flag/channel/endpoint cleanup)
