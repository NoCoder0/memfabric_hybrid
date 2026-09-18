#!/bin/bash
# bench_monitor.sh —— 纯监控脚本：挂着采 hostrdma_batch_bench 运行期间的关键指标，
# 被监控进程一退出就结束本次监控并打印汇总。
#
# 不启动、不配置 bench，只监控。容器里直接跑，不需要任何参数：
#   bash bench_monitor.sh                # 等 bench 进程出现，然后监控到它退出
#   bash bench_monitor.sh 12345          # 监控指定 pid
#   Ctrl-C 也可以提前结束并打印汇总
#
# 可选环境变量：
#   PATTERN=hostrdma_batch_bench   进程匹配串（默认）
#   INTERVAL=0.2                   采样间隔秒（默认 0.2；基准循环通常只有几百毫秒，间隔别太大）
#   WAIT_SEC=300                   等进程出现的最长秒数（默认 300）
#   OUT=<file>                     采样明细落盘路径（默认 bench_monitor_<时间>.log）
#   NO_COUNTERS=1                  不读网卡计数
#
# 采什么（每 tick）：
#   1) 业务 QP：数量 + 分别在哪个 device（rdma res show qp，剔掉 ib_core）
#   2) 每个线程：当前所在核(psr) + 本 tick 的 CPU%（用 /proc/<pid>/task/<tid>/stat 的
#      utime+stime 做增量，反映"此刻在不在干活"；ps 的 pcpu 是进程生命周期平均值，
#      在这种"进程活十几秒、基准循环只有几百毫秒"的场景里没有参考价值）
#   3) 网卡：各 RDMA 口 port_xmit_data/port_rcv_data 累计值（结束时给增量，×4 = 字节）
#
# 结束时汇总：QP 峰值与分布、每个线程的 CPU 峰值（含峰值出现的 tick）与核冲突、
#            各口流量增量（出现负值会告警：计数被复位/回滚，本次增量不可用）。

set -uo pipefail

PATTERN="${PATTERN:-hostrdma_batch_bench}"
INTERVAL="${INTERVAL:-0.2}"
WAIT_SEC="${WAIT_SEC:-300}"
NO_COUNTERS="${NO_COUNTERS:-}"
OUT="${OUT:-bench_monitor_$(date +%Y%m%d_%H%M%S).log}"

say() { printf '%s\n' "$*"; }

have_rdma=0; command -v rdma >/dev/null 2>&1 && have_rdma=1
CLK_TCK="$(getconf CLK_TCK 2>/dev/null || echo 100)"

snap_counters() {
    [ -n "$NO_COUNTERS" ] && return
    local p d
    for p in /sys/class/infiniband/*/ports/1; do
        d="$(echo "$p" | cut -d/ -f5)"
        [ -r "$p/counters/port_xmit_data" ] || continue
        echo "$d $(cat "$p/counters/port_xmit_data") $(cat "$p/counters/port_rcv_data")"
    done
}

# QP 按 device 聚合: 输出 "<n> <dev>" 多行
snap_qp() {
    [ "$have_rdma" = 1 ] || return
    rdma res show qp 2>/dev/null | grep -v 'ib_core' | grep -v '^$' |
        awk '{print $2}' | cut -d/ -f1 | sort | uniq -c | awk '{print $1" "$2}'
}

# ---------- 1. 定位被监控进程 ----------
PID="${1:-}"
if [ -z "$PID" ]; then
    say "[monitor] 等待 '${PATTERN}' 进程出现（最多 ${WAIT_SEC}s）..."
    waited=0
    while [ "$waited" -lt "$WAIT_SEC" ]; do
        PID="$(pgrep -f "$PATTERN" 2>/dev/null | grep -vw "$$" | head -1)"
        [ -n "$PID" ] && break
        sleep 1
        waited=$((waited + 1))
    done
    [ -n "$PID" ] || { say "[monitor] 等不到 '${PATTERN}'，退出"; exit 1; }
fi
kill -0 "$PID" 2>/dev/null || { say "[monitor] pid ${PID} 不存在，退出"; exit 1; }
say "[monitor] 开始监控 pid=${PID} comm=$(cat /proc/$PID/comm 2>/dev/null)  间隔=${INTERVAL}s"
say "[monitor] 明细写入: ${OUT}"

BEFORE="$(snap_counters)"
QP_MAX=0; QP_DIST=""
TICKS=0
declare -A PREV_TICKS=()          # tid -> 上次 utime+stime
declare -A MAX_CPU=()             # tid -> 观察到的 CPU% 峰值
declare -A MAX_CPU_TICK=()        # tid -> 峰值出现在第几个 tick
declare -A LAST_CORE=()           # tid -> 最后一次看到的核
declare -A LAST_COMM=()           # tid -> 最后一次看到的线程名
PREV_NS=0
START_NS="$(date +%s%N)"
STOP=0
trap 'STOP=1' INT TERM

# ---------- 2. 采到进程退出 ----------
while kill -0 "$PID" 2>/dev/null; do
    [ "$STOP" = 1 ] && { say "[monitor] 收到中断信号，停止采样"; break; }
    TICKS=$((TICKS + 1))
    NOW_NS="$(date +%s%N)"
    DT_NS=$((NOW_NS - PREV_NS)); [ "$PREV_NS" = 0 ] && DT_NS=0

    # 一次 ps 拿全部线程的核与名字
    CORES="$(ps -T -p "$PID" -o tid=,psr=,comm= 2>/dev/null)"
    unset CORE_OF COMM_OF   # bash 4.4+ 支持 unset 整个数组；旧版本下一行也能覆盖
    declare -A CORE_OF=() COMM_OF=()
    while read -r tid psr comm; do
        [ -z "${tid:-}" ] && continue
        CORE_OF[$tid]="$psr"; COMM_OF[$tid]="$comm"
    done <<< "$CORES"

    # 读每个线程的 CPU 累计 tick
    declare -A CUR=()
    for t in /proc/$PID/task/*; do
        [ -r "$t/stat" ] || continue
        tid="${t##*/}"
        val="$(awk '{ sub(/^[^)]*\) /, ""); print $12 + $13 }' "$t/stat" 2>/dev/null)"
        [ -n "$val" ] && CUR[$tid]="$val"
    done

    {
        echo "=== tick ${TICKS}  $(date +%H:%M:%S) ==="
        echo "-- qp(按 device 聚合) --"
        qp_lines="$(snap_qp)"
        if [ -n "$qp_lines" ]; then echo "$qp_lines"; else echo "(无 / 不可用)"; fi
        echo "-- threads (tid core cpu%_this_tick comm) --"
        for tid in "${!CUR[@]}"; do
            cpu="-"
            prev="${PREV_TICKS[$tid]:-}"
            if [ -n "$prev" ] && [ "$DT_NS" -gt 0 ]; then
                d=$((CUR[$tid] - prev)); [ "$d" -lt 0 ] && d=0
                cpu=$(( d * 100000000000 / (CLK_TCK * DT_NS) ))
                prevmax="${MAX_CPU[$tid]:-0}"
                [ "$cpu" -gt "$prevmax" ] && { MAX_CPU[$tid]="$cpu"; MAX_CPU_TICK[$tid]="$TICKS"; }
            fi
            LAST_CORE[$tid]="${CORE_OF[$tid]:-?}"
            LAST_COMM[$tid]="${COMM_OF[$tid]:-?}"
            echo "  $tid ${CORE_OF[$tid]:-?} ${cpu} ${COMM_OF[$tid]:-?}"
        done
    } >> "$OUT"

    # 记账 QP 峰值/分布
    if [ -n "${qp_lines:-}" ]; then
        tot=0; this_dist=""
        while read -r cnt dev; do
            [ -z "${dev:-}" ] && continue
            tot=$((tot + cnt)); this_dist="${this_dist}${dev}=${cnt} "
        done <<< "$qp_lines"
        [ "$tot" -gt "$QP_MAX" ] && { QP_MAX="$tot"; QP_DIST="$this_dist"; }
    fi

    for tid in "${!CUR[@]}"; do PREV_TICKS[$tid]="${CUR[$tid]}"; done
    FINAL_TIDS=("${!CUR[@]}")   # 最后一次采样时还活着的线程（汇总里的线程数/核分布以它为准）
    PREV_NS="$NOW_NS"
    sleep "$INTERVAL"
done

ELAPSED_MS=$(( ($(date +%s%N) - START_NS) / 1000000 ))

# ---------- 3. 汇总 ----------
AFTER="$(snap_counters)"

say ""
say "================ 监控汇总 ================"
say "进程 pid=${PID}  时长=$((ELAPSED_MS / 1000)).$((ELAPSED_MS % 1000 / 100))s  采样=${TICKS} 次  明细=${OUT}"
say ""
say "--- 业务 QP（峰值）---"
if [ "$have_rdma" != 1 ]; then
    say "容器里没有 rdma 工具（rdma-core），采不到 QP"
elif [ "$QP_MAX" -gt 0 ]; then
    say "峰值业务 QP 数 = ${QP_MAX}   分布: ${QP_DIST}"
    devs="$(echo "$QP_DIST" | tr ' ' '\n' | grep '=' | cut -d= -f1 | grep -c .)"
    if [ "$devs" -ge 2 ]; then
        say "判断: QP 分布在 ${devs} 个 device 上 → 多条 channel 落在多张卡（按 service 数核对是否齐全）"
    else
        say "判断: 业务 QP 只落在 1 个 device 上 → 数据面只用了一张卡"
    fi
else
    say "监控期间没采到业务 QP"
fi
say ""
say "--- 线程 CPU 峰值（按峰值降序；tick 表示峰值出现在第几次采样）---"
if [ "${#MAX_CPU[@]}" -eq 0 ]; then
    say "(没采到 CPU 数据；进程存活时间短于两个采样间隔时正常)"
else
    for tid in $(for k in "${!MAX_CPU[@]}"; do echo "${MAX_CPU[$k]} $k"; done | sort -rn | awk '{print $2}'); do
        printf '  tid=%-8s core=%-4s cpu_peak=%3s%%  @tick %-4s  %s\n' \
            "$tid" "${LAST_CORE[$tid]:-?}" "${MAX_CPU[$tid]}" "${MAX_CPU_TICK[$tid]:-?}" "${LAST_COMM[$tid]:-?}"
    done
    # 核冲突：只看最后一次采样时还活着的线程（跨 tick 累计会把已退出的线程也算进来，口径会失真）
    FINAL_TIDS=("${FINAL_TIDS[@]:-}")
    cores_now=""
    for tid in "${FINAL_TIDS[@]}"; do
        [ -z "$tid" ] && continue
        c="${LAST_CORE[$tid]:-?}"; [ "$c" = "?" ] && continue
        cores_now="${cores_now}${c}\n"
    done
    dup="$(printf "$cores_now" | sort | uniq -d | tr '\n' ' ')"
    ncores="$(printf "$cores_now" | sort -u | grep -c .)"
    say "最后存活线程数=${#FINAL_TIDS[@]}  落在 ${ncores} 个不同核上"
    if [ -n "$dup" ]; then
        say "⚠ 有核被多个线程共用（可能抢核）: ${dup}"
    else
        say "无核冲突（每个线程一个独立核）"
    fi
    say "判读: 核心看 cpu_peak —— 忙轮询 worker 应长期偏高；提交 worker 应在基准循环那几次采样里冲高"
fi
say ""
say "--- 网卡流量增量（×4 = 字节）---"
if [ -n "$NO_COUNTERS" ]; then
    say "(已用 NO_COUNTERS=1 关闭)"
elif [ -z "$BEFORE" ]; then
    say "(读不到 /sys/class/infiniband/*/ports/1/counters)"
else
    NEG=0
    while read -r d x r; do
        [ -z "${d:-}" ] && continue
        o="$(echo "$AFTER" | awk -v d="$d" '$1==d{print $2" "$3}')"
        [ -z "$o" ] && continue
        ox="$(echo "$o" | awk '{print $1}')"
        orr="$(echo "$o" | awk '{print $2}')"
        dx=$((x - ox)); dr=$((r - orr))
        [ "$dx" -lt 0 ] || [ "$dr" -lt 0 ] && NEG=1
        printf '%-16s xmit=%14d (%d B)   rcv=%14d (%d B)\n' "$d" "$dx" "$((dx * 4))" "$dr" "$((dr * 4))"
    done <<< "$BEFORE"
    if [ "$NEG" = 1 ]; then
        say "⚠ 出现负增量：计数在监控期间被复位/回滚了，本次流量数据不可用（检查是不是换了命名空间/重启过设备）"
    else
        say "判读: 各口增量是否均匀 —— 均匀 = 数据面分摊到多张卡；只有一个口在涨 = 只走一张卡"
    fi
fi
say "=========================================="
