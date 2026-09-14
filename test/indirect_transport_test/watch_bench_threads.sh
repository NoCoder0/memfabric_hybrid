#!/bin/bash
# =============================================================================
# 独立诊断脚本（与任何工程代码无关，只需要一个"二进制名"）
#
# 作用：先启动本脚本，它会在后台等目标进程出现 -> 自动抓住 PID -> 在运行期高频
#       采样该进程所有线程的 落核(psr) / nice / CPU / NUMA 归属，退出后自动汇总，
#       并直接标出"哪个线程和主线程共物理核"。
#
# 顺序无所谓：脚本可以先起，也可以等 bench 已经在跑再起。
#
# 用法：
#   bash watch_bench_threads.sh [选项]
#
# 选项：
#   --name=<名字>        目标二进制名（默认 hostrdma_batch_bench）
#   --pid=<pid>          跳过等待，直接监控已知 PID
#   --timeout=<秒>       等待进程出现的超时（默认 60）
#   --interval-ms=<ms>   采样间隔（默认 20）
#   --outdir=<目录>      输出目录（默认 ./bench_diag_<时间>）
#   --runs=<N>           连续观测 N 次运行（默认 1），每次输出到 outdir/runN/
#   -h|--help            帮助
#
# 例子：
#   bash watch_bench_threads.sh
#   bash watch_bench_threads.sh --name=hostrdma_batch_bench --interval-ms=10
#   bash watch_bench_threads.sh --runs=3        # 起一次脚本，连测 3 次 bench
# =============================================================================

NAME="hostrdma_batch_bench"
TIMEOUT_S=60
INTERVAL_MS=20
OUTDIR=""
PID_GIVEN=""
RUNS=1

for arg in "$@"; do
    case "$arg" in
        --name=*)        NAME="${arg#*=}" ;;
        --pid=*)         PID_GIVEN="${arg#*=}" ;;
        --timeout=*)     TIMEOUT_S="${arg#*=}" ;;
        --interval-ms=*) INTERVAL_MS="${arg#*=}" ;;
        --outdir=*)      OUTDIR="${arg#*=}" ;;
        --runs=*)        RUNS="${arg#*=}" ;;
        -h|--help)
            sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "[WARN] 忽略未知参数: $arg"
            ;;
    esac
done

BASE=$(basename "${NAME}")
SHORT="${BASE:0:15}"   # Linux comm 最长 15 字符，必须按截断名匹配
[ -n "${OUTDIR}" ] || OUTDIR="./bench_diag_$(date +%m%d_%H%M%S)"
mkdir -p "${OUTDIR}"

echo "=============================================================="
echo " 目标进程名 : ${BASE}  (comm 截断为 '${SHORT}')"
echo " 采样间隔   : ${INTERVAL_MS} ms"
echo " 等待超时   : ${TIMEOUT_S} s"
echo " 观测次数   : ${RUNS}"
echo " 输出目录   : ${OUTDIR}"
echo "=============================================================="
echo "[提示] 现在去起 bench 即可，脚本已在等待..."

# ---------- 静态信息（只采一次，与进程无关） ----------
lscpu -e=CPU,CORE,SOCKET,NODE,ONLINE >"${OUTDIR}/lscpu.txt" 2>&1
lscpu >"${OUTDIR}/lscpu_full.txt" 2>&1
nproc >"${OUTDIR}/nproc.txt" 2>&1
numactl -H >"${OUTDIR}/numactl_H.txt" 2>&1 || echo "(numactl -H 不可用)" >"${OUTDIR}/numactl_H.txt"
awk 'NR==1 { for (i = 1; i <= NF; i++) { if ($i == "CPU") c = i; if ($i == "CORE") k = i } next }
     c && k { print $c, $k }' "${OUTDIR}/lscpu.txt" >"${OUTDIR}/cpu2core.txt"

# 系统级限制：nproc 与亲和掩码不一致时，答案通常在这里
{
    echo "== CPU 数量 =="
    echo "nproc        : $(nproc 2>/dev/null)"
    echo "nproc --all  : $(nproc --all 2>/dev/null)"
    echo "online cpus  : $(cat /sys/devices/system/cpu/online 2>/dev/null)"
    echo
    echo "== 本脚本 shell 自身的亲和（对照进程的 Cpus_allowed_list） =="
    grep -E '^(Cpus_allowed_list|Mems_allowed_list)' /proc/self/status 2>/dev/null
    taskset -pc $$ 2>/dev/null
    echo
    echo "== cgroup 限制（关键：是否只给了 1 个 CPU） =="
    for f in /sys/fs/cgroup/cpu.max /sys/fs/cgroup/cpuset.cpus.effective \
             /sys/fs/cgroup/cpu/cpu.cfs_quota_us /sys/fs/cgroup/cpu/cpu.cfs_period_us \
             /sys/fs/cgroup/cpuset/cpuset.cpus; do
        [ -r "$f" ] && echo "$(basename "$f") : $(cat "$f")"
    done
    head -5 /proc/1/cgroup 2>/dev/null
    echo
    echo "== OpenMP / 线程相关环境变量（GNU nproc 会取 OMP_NUM_THREADS 与亲和掩码的较小值） =="
    env | grep -i -E 'OMP|MKL|THREAD|KMP|GOMP' || echo "(无)"
} >"${OUTDIR}/sysenv.txt" 2>&1

# 按名字找 PID：pgrep 匹配命令行 -> 再用 comm 前缀复核，避免误抓
find_pid_by_name()
{
    local pid c
    for pid in $(pgrep -f -- "${BASE}" 2>/dev/null); do
        [ "${pid}" = "$$" ] && continue
        c=$(cat "/proc/${pid}/comm" 2>/dev/null) || continue
        case "${c}" in
            "${SHORT}"*) printf '%s\n' "${pid}"; return 0 ;;
        esac
    done
    return 1
}

# 轮询等待进程出现（间隔 5ms，尽量抓住短命进程）
wait_for_pid()
{
    local deadline=$(( $(date +%s) + TIMEOUT_S ))
    local p
    while [ "$(date +%s)" -le "${deadline}" ]; do
        if p=$(find_pid_by_name); then
            printf '%s\n' "${p}"
            return 0
        fi
        sleep 0.005
    done
    return 1
}

# 汇总某一次运行
summarize()
{
    local rundir="$1" pid="$2" proc_mask="$3" proc_mems="$4" proc_threads="$5" samples="$6" count="$7" rc="$8"
    local summary="${rundir}/summary.txt"
    {
        echo "binary=${BASE} pid=${pid} samples=${count} exit=${rc}"
        echo "进程 Cpus_allowed     : ${proc_mask:-(未采到)}"
        echo "进程 Mems_allowed     : ${proc_mems:-(未采到)}"
        echo "进程 Threads          : ${proc_threads:-(未采到)}"
        echo "nproc                 : $(cat "${OUTDIR}/nproc.txt" 2>/dev/null)"
        echo "SMT                   : $(grep -i -m1 'per core' "${OUTDIR}/lscpu_full.txt" 2>/dev/null)"
        echo
        echo "===== 判据 ====="
        echo "1) 进程 Cpus_allowed 是否等于你绑定/限制的范围（是 => 子线程继承了该 mask）"
        echo "2) RDMAWkr*/RDMAEvent* 的 maxCPU%：~0 => 忙轮询线程没在跑；~100 => 在跑但完成仍晚到"
        echo "3) SHARED-CORE 段：主线程与哪些线程共物理核"
        echo
        echo "===== 各线程观测到的 CPU / 物理核（按 tid 聚合，ni 取观测最小，cpu 取观测最大） ====="
        awk -v main="${pid}" '
            FNR == NR { core[$1] = $2; next }
            $1 == "----" { next }
            NF < 5 { next }
            {
                tid = $1
                cpus[tid][$2] = 1
                cores[tid][core[$2]] = 1
                if (!(tid in ni) || $3 + 0 < ni[tid] + 0 || ni[tid] == "") ni[tid] = $3
                comm[tid] = $5
                if (NF >= 6 && (!(tid in maxcpu) || $6 + 0 > maxcpu[tid] + 0)) maxcpu[tid] = $6 + 0
                if (tid == main) ismain = 1
            }
            END {
                if (main in cpus) {
                    cl = ""; for (x in cpus[main]) cl = cl (cl == "" ? "" : ",") x
                    rl = ""; for (x in cores[main]) rl = rl (rl == "" ? "" : ",") x
                    printf "tid=%-8s ni=%-5s cpu=%-7s comm=%-24s cpus=[%s] cores=[%s]  <== MAIN\n", main,
                           (main in ni ? ni[main] : "-"), (main in maxcpu ? maxcpu[main] "%" : "-"), comm[main], cl, rl
                }
                for (t in cpus) {
                    if (t == main) continue
                    cl = ""; for (x in cpus[t]) cl = cl (cl == "" ? "" : ",") x
                    rl = ""; for (x in cores[t]) rl = rl (rl == "" ? "" : ",") x
                    printf "tid=%-8s ni=%-5s cpu=%-7s comm=%-24s cpus=[%s] cores=[%s]\n", t,
                           (t in ni ? ni[t] : "-"), (t in maxcpu ? maxcpu[t] "%" : "-"), comm[t], cl, rl
                }
                print ""
                print "===== 关键线程 CPU%（判断忙轮询 worker 到底在不在跑） ====="
                printf "%-9s %-22s %-9s %s\n", "tid", "comm", "maxCPU%", "cpus"
                for (t in cpus) {
                    c = comm[t]
                    if (t == main || c ~ /^RDMAWkr/ || c ~ /^RDMAEvent/ || c ~ /^HCOMHb/ || c ~ /hostrdma/) {
                        cl = ""; for (x in cpus[t]) cl = cl (cl == "" ? "" : ",") x
                        printf "%-9s %-22s %-9s %s\n", t, c, (t in maxcpu ? maxcpu[t] : "-"), cl
                    }
                }
                print "  ^ 主线程(hostrdma_batch_)在 cont 接收端是纯自旋，~100% 属正常，不用管；"
                print "    RDMAWkr*/RDMAEvent* 若 ~0% => 忙轮询线程根本没在跑；若 ~100% => 在跑但完成仍晚到"
                print ""
                if (ismain) {
                    print "===== 与主线程共物理核的线程 ====="
                    n = 0
                    for (o in cores) {
                        if (o == main) continue
                        s = 0
                        for (c in cores[o]) if (c in cores[main]) s = 1
                        if (s) { printf "SHARED-CORE: MAIN(tid=%s) <-> tid=%s comm=%s\n", main, o, comm[o]; n++ }
                    }
                    if (n == 0) print "(未观测到共核)"
                }
            }' "${OUTDIR}/cpu2core.txt" "${samples}"
        if [ -s "${rundir}/schedstat.txt" ]; then
            echo
            echo "===== 每线程 排队时间增量（wait = 要跑却在 runqueue 等；接近 0 => 没等过） ====="
            awk '$1 == "S" && NF >= 6 {
                     t = $2; comm[t] = $6
                     if (!(t in seen)) { r0[t] = $3; w0[t] = $4; seen[t] = 1 }
                     r1[t] = $3; w1[t] = $4
                 }
                 END {
                     printf "%-9s %-22s %-12s %-12s\n", "tid", "comm", "run+ms", "wait+ms"
                     for (t in r1)
                         printf "%-9s %-22s %-12.0f %-12.0f\n", t, comm[t],
                                (r1[t] - r0[t]) / 1e6, (w1[t] - w0[t]) / 1e6
                 }' "${rundir}/schedstat.txt"
            echo "  ^ 若 hostrdma_batch_(MAIN) 的 wait+ms 很大(~轮次*4ms) => 它醒来后在排队等 CPU"
            echo "    若 MAIN 的 wait+ms 很小、run+ms 很大 => 它在烧 CPU，不是等（那就要换方向查）"
        fi
    } >"${summary}" 2>&1
    echo "[diag] 汇总: ${summary}"
}

SLEEP_S=$(awk -v ms="${INTERVAL_MS}" 'BEGIN { printf "%.3f", ms / 1000 }')

for run in $(seq 1 "${RUNS}"); do
    RUNDIR="${OUTDIR}"
    [ "${RUNS}" -gt 1 ] && RUNDIR="${OUTDIR}/run${run}"
    mkdir -p "${RUNDIR}"

    echo
    echo "---------- run ${run}/${RUNS} ----------"

    if [ -n "${PID_GIVEN}" ]; then
        PID="${PID_GIVEN}"
        [ -d "/proc/${PID}" ] || { echo "[ERROR] 指定的 pid ${PID} 不存在"; exit 1; }
        echo "[diag] 使用指定 pid = ${PID}"
    else
        PID=$(wait_for_pid) || {
            echo "[ERROR] ${TIMEOUT_S}s 内没等到进程 '${BASE}' 出现"
            exit 1
        }
        echo "[diag] 已捕获 pid = ${PID}"
    fi
    echo "${PID}" >"${RUNDIR}/pid.txt"

    # 进程还在，先把 /proc 关键信息抢下来
    PROC_MASK=$(grep -m1 '^Cpus_allowed_list' "/proc/${PID}/status" 2>/dev/null)
    PROC_MEMS=$(grep -m1 '^Mems_allowed_list' "/proc/${PID}/status" 2>/dev/null)
    PROC_THREADS=$(grep -m1 '^Threads' "/proc/${PID}/status" 2>/dev/null)
    {
        echo "===== /proc/${PID}/status 摘要 ====="
        grep -E '^(Name|Pid|Threads|Cpus_allowed_list|Mems_allowed_list)' "/proc/${PID}/status" 2>&1
        echo
        echo "===== 各线程 Cpus_allowed_list（确认子线程是否继承） ====="
        grep -H "Cpus_allowed_list" "/proc/${PID}/task/"*/status 2>&1
        echo
        echo "===== taskset -pc ${PID} ====="
        taskset -pc "${PID}" 2>&1
        echo
        echo "===== 直读 /proc/<tid>/stat 第39列(processor)，交叉验证 ps 的 psr ====="
        for t in "/proc/${PID}/task/"*; do
            [ -r "${t}/stat" ] || continue
            printf "tid=%-8s cpu(stat39)=%-4s comm=%s\n" "$(basename "${t}")" \
                   "$(awk '{print $39}' "${t}/stat" 2>/dev/null)" "$(cat "${t}/comm" 2>/dev/null)"
        done
    } >"${RUNDIR}/masks.txt" 2>&1

    SAMPLES="${RUNDIR}/samples.txt"
    : >"${SAMPLES}"
    NUMASTAT_LOG="${RUNDIR}/numastat.txt"
    : >"${NUMASTAT_LOG}"
    SCHED_LOG="${RUNDIR}/schedstat.txt"
    : >"${SCHED_LOG}"

    echo "[diag] 开始采样（${PROC_THREADS:-Threads: ?}）..."
    CNT=0
    while [ -d "/proc/${PID}" ]; do
        ps -Lp "${PID}" -o tid=,psr=,ni=,pri=,comm=,pcpu= --no-headers >>"${SAMPLES}" 2>/dev/null
        # 每线程 schedstat：run/wait(ns) —— wait 就是"明明要跑却在 runqueue 里等"的累计时间
        for t in "/proc/${PID}/task/"*; do
            [ -r "${t}/schedstat" ] || continue
            printf "S %s %s %s\n" "$(basename "${t}")" "$(cat "${t}/schedstat" 2>/dev/null)" \
                   "$(cat "${t}/comm" 2>/dev/null)" >>"${SCHED_LOG}"
        done
        echo "---- t=${CNT} ----" >>"${SAMPLES}"
        CNT=$((CNT + 1))
        if [ $((CNT % 25)) -eq 0 ]; then
            echo "---- t=${CNT} ----" >>"${NUMASTAT_LOG}"
            numastat -p "${PID}" >>"${NUMASTAT_LOG}" 2>&1 || true
            echo "[watch t=${CNT}] $(ps -eLo pcpu,psr,comm --no-headers 2>/dev/null | grep -E 'RDMAWkr|RDMAEvent|HCOMHb' | tr -s ' ' | tr '\n' ' ')"
        fi
        sleep "${SLEEP_S}"
    done
    RC="n/a"   # 进程不是本脚本的子进程，拿不到真实退出码
    echo "[diag] pid=${PID} 已退出，采样 ${CNT} 次"

    summarize "${RUNDIR}" "${PID}" "${PROC_MASK}" "${PROC_MEMS}" "${PROC_THREADS}" "${SAMPLES}" "${CNT}" "${RC}"

    # 多次观测之间留点余量，避免把上一次的残留线程算进来
    [ "${run}" -lt "${RUNS}" ] && sleep 0.2
done

echo
echo "=============================================================="
echo "[diag] 全部完成，输出目录: ${OUTDIR}"
echo "[diag]   summary.txt  汇总（含 SHARED-CORE 结论）"
echo "[diag]   masks.txt    进程/线程亲和掩码"
echo "[diag]   samples.txt  运行期落核原始采样"
echo "[diag]   numastat.txt 内存落点"
echo "[diag]   lscpu.txt    CPU/CORE/NODE 拓扑"
echo "=============================================================="
