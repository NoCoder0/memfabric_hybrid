#!/usr/bin/env python3
# SPDX-License-Identifier: MulanPSL-2.0
"""Run one device process to a full log; save identity/exit, then reduce after exit."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import time


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def command(argv):
    p = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if p.returncode:
        raise RuntimeError("command failed: " + repr(argv) + ": " + p.stderr.decode(errors="replace"))
    return p.stdout


def source_identity(path):
    return {
        "path": str(Path(path).resolve()),
        "commit": command(["git", "-C", path, "rev-parse", "HEAD"]).decode().strip(),
        "status": command(["git", "-C", path, "status", "--short"]).decode().splitlines(),
        "diff_sha256": hashlib.sha256(command(["git", "-C", path, "diff", "HEAD", "--binary"])).hexdigest(),
    }


def proc_snapshot(pid):
    """Outside target process. Retain last observed affinity per thread, not event samples."""
    affinities, libraries = {}, set()
    try:
        for thread in Path("/proc", str(pid), "task").iterdir():
            affinities[thread.name] = sorted(os.sched_getaffinity(int(thread.name)))
        for line in Path("/proc", str(pid), "maps").read_text().splitlines():
            fields = line.split(None, 5)
            if len(fields) == 6 and any(n in fields[-1] for n in ("libhcom.so", "libmf_", "libboundscheck")):
                libraries.add(fields[-1])
    except (OSError, ProcessLookupError):
        pass
    return affinities, libraries


def finish_identity(identity, log_path, manifest):
    from analyze_hostrdma_trace import read_records

    records = read_records(log_path)
    actual = [r["path"] for r in records if r.get("record_type") == "mf_trace_library"]
    for path in actual:
        resolved = str(Path(path).resolve())
        identity["loaded_libraries"].append({"path": resolved, "sha256": digest(resolved)})
    common = manifest["paths"]["STAGE_HCOM_SHARED"]
    expected_sha = manifest["artifacts"][str(Path(common).resolve())]
    hcom = [r for r in identity["loaded_libraries"] if "libhcom.so" in r["path"]]
    identity["artifact_verified"] = (
        bool(hcom) and all(r["sha256"] == expected_sha for r in hcom) if identity["app"] == "mf" else True
    )
    identity["nic_ports"] = []
    for qp in records:
        if qp.get("record_type") != "hcom_qp_identity":
            continue
        root = Path("/sys/class/infiniband", qp["device"], "ports", str(qp["port"]))
        port = {k: (root / k).read_text().strip() for k in ("rate", "state", "link_layer") if (root / k).exists()}
        port.update(device=qp["device"], port=qp["port"])
        identity["nic_ports"].append(port)
    return records


def baseline_result(identity, records, log_path):
    if identity["app"] == "sgl":
        rows = [r for r in records if r.get("schema_version") == 8 and r.get("role") == identity["role"]]
    else:
        text = Path(log_path).read_text(errors="replace")
        rows = [
            line.strip() for line in text.splitlines() if re.match(r"cont receiver (e2e_us:|verify=|scatter_us:)", line)
        ]
        if identity["role"] == "local" and ("cont receiver verify=OK" not in text or "TIMEOUT" in text):
            rows.append("FAIL: missing successful local verification or timeout")
    failed = any("FAIL" in str(row) or "TIMEOUT" in str(row) for row in rows)
    return {
        "format": "stage-baseline-v1",
        "identity": identity,
        "results": rows,
        "status": "ok"
        if identity["exit_code"] == 0
        and identity["artifact_verified"]
        and not failed
        and (rows or identity["role"] == "remote")
        else "incomplete",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", choices=["mf", "sgl"], required=True)
    parser.add_argument("--role", choices=["local", "remote"], required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--comparison-id", required=True)
    parser.add_argument("--repo", required=True)
    parser.add_argument("--hcom-repo", required=True)
    parser.add_argument("--build-manifest", required=True)
    parser.add_argument("--output", required=True, help="new output prefix; existing logs are never overwritten")
    parser.add_argument("--trace-rounds", type=int, default=3)
    parser.add_argument("--trace-off", action="store_true")
    parser.add_argument("--baseline", help="optional preceding trace-off compact result from this exact binary")
    parser.add_argument("argv", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    argv = args.argv[1:] if args.argv[:1] == ["--"] else args.argv
    if not argv or platform.system() != "Linux":
        parser.error("requires Linux and executable after --")
    prefix = Path(args.output).resolve()
    prefix.parent.mkdir(parents=True, exist_ok=True)
    for suffix in (".log", ".identity.json", ".compact.json", ".analysis.stderr"):
        if Path(str(prefix) + suffix).exists():
            parser.error("output exists: " + str(prefix) + suffix)
    executable = shutil.which(argv[0])
    if not executable:
        parser.error("executable not found")
    manifest = json.loads(Path(args.build_manifest).read_text())
    binary_path = str(Path(executable).resolve())
    if manifest["artifacts"].get(binary_path) != digest(binary_path):
        parser.error("executable is not the one recorded by the final build manifest; pass the binary directly")
    for repo, key in [(args.hcom_repo, "hcom_source"), (args.repo, args.app + "_source")]:
        current = source_identity(repo)
        if any(current[k] != manifest[key][k] for k in ("commit", "diff_sha256")):
            parser.error(key + " changed since build manifest; rebuild before capture")
    identity = {
        "run_id": args.run_id,
        "comparison_id": args.comparison_id,
        "app": args.app,
        "role": args.role,
        "host": platform.node(),
        "kernel": platform.release(),
        "argv": argv,
        "executable": str(Path(executable).resolve()),
        "executable_sha256": digest(executable),
        "app_source": source_identity(args.repo),
        "hcom_source": source_identity(args.hcom_repo),
        "build_manifest_sha256": digest(args.build_manifest),
        "hcom_artifacts": {
            path: sha
            for path, sha in manifest["artifacts"].items()
            if Path(path).name.startswith(("libhcom", "libboundscheck"))
        },
        "trace": not args.trace_off,
        "trace_rounds": 0 if args.trace_off else args.trace_rounds,
        "environment": {
            k: v
            for k, v in os.environ.items()
            if (
                k.startswith(("MF_", "RDMA_600_", "HCOM_"))
                or k in ("LD_LIBRARY_PATH", "OMP_NUM_THREADS", "ASCEND_MF_LOG_LEVEL")
            )
            and not any(s in k.upper() for s in ("SECRET", "TOKEN", "PASSWORD", "PRIVATE_KEY"))
        },
        "observer_affinity": sorted(os.sched_getaffinity(0)),
    }
    if args.baseline:
        baseline = json.loads(Path(args.baseline).read_text())
        previous = baseline["identity"]
        if (
            baseline["status"] != "ok"
            or previous["executable_sha256"] != identity["executable_sha256"]
            or (previous["comparison_id"], previous["app"], previous["role"])
            != (args.comparison_id, args.app, args.role)
        ):
            parser.error("baseline must be successful and use this binary/comparison/role")
        identity["baseline"] = {
            "run_id": previous["run_id"],
            "compact_sha256": digest(args.baseline),
            "trace": previous["trace"],
            "results": baseline["results"],
        }
    identity_path = Path(str(prefix) + ".identity.json")
    identity_path.write_text(json.dumps(identity, indent=2))
    affinities, libraries = {}, set()
    with open(str(prefix) + ".log", "xb") as log:
        process = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT)
        while process.poll() is None:
            a, libs = proc_snapshot(process.pid)
            affinities.update(a)
            libraries.update(libs)
            time.sleep(0.25)
        identity["exit_code"] = process.wait()
    identity["thread_affinity_last_observed"] = affinities
    identity["loaded_libraries"] = [{"path": p, "sha256": digest(p)} for p in sorted(libraries) if Path(p).is_file()]
    identity["log_sha256"] = digest(str(prefix) + ".log")
    identity_path.write_text(json.dumps(identity, indent=2))
    records = finish_identity(identity, str(prefix) + ".log", manifest)
    identity_path.write_text(json.dumps(identity, indent=2))
    if args.trace_off:
        baseline = baseline_result(identity, records, str(prefix) + ".log")
        Path(str(prefix) + ".compact.json").write_text(json.dumps(baseline, separators=(",", ":")))
        print("Saved complete trace-off log and exit/identity: " + str(prefix))
        return identity["exit_code"] or (0 if baseline["status"] == "ok" else 1)
    analyzer = Path(__file__).with_name("compare_stage_trace.py")
    with open(str(prefix) + ".compact.json", "xb") as out, open(str(prefix) + ".analysis.stderr", "xb") as err:
        analysis = subprocess.run(
            [
                sys.executable,
                str(analyzer),
                "reduce",
                str(prefix) + ".log",
                "--identity",
                str(identity_path),
                "--app",
                args.app,
                "--role",
                args.role,
            ],
            stdout=out,
            stderr=err,
        )
    compact_path = Path(str(prefix) + ".compact.json")
    if compact_path.stat().st_size > 16000:
        full = json.loads(compact_path.read_text())
        for row in full["rounds"]:
            split = dict(full, rounds=[row])
            split_path = Path(str(prefix) + ".round-" + str(row["round"]) + ".compact.json")
            split_path.write_text(json.dumps(split, separators=(",", ":")))
        print("Large result: per-round files also written; keep the full compact and raw log on the device.")
    print(
        json.dumps(
            {
                "prefix": str(prefix),
                "process_exit": identity["exit_code"],
                "analysis_exit": analysis.returncode,
                "compact_bytes": Path(str(prefix) + ".compact.json").stat().st_size,
            }
        )
    )
    return identity["exit_code"] or analysis.returncode


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError, KeyError) as error:
        print("ERROR: " + str(error), file=sys.stderr)
        sys.exit(2)
