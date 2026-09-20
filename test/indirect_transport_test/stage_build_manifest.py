#!/usr/bin/env python3
# SPDX-License-Identifier: MulanPSL-2.0
"""Select exact MF-produced HCOM artifacts and record their hashes/link evidence."""

import json
from pathlib import Path
import shlex
import shutil
import sys

from capture_stage_run import digest, source_identity


def unique(paths, label):
    values = sorted({p.resolve() for p in paths if p.exists()})
    if len(values) != 1:
        raise RuntimeError("expected one " + label + ": " + repr(values))
    return values[0]


def main():
    mf, ubs, sgl = [Path(p).resolve() for p in sys.argv[1:4]]
    # The archive in the build tree is authoritative; output can contain package copies.
    archive = unique((mf / "build").rglob("libhcom_static.a"), "built HCOM archive")
    shared = unique(archive.parent.glob("libhcom.so*"), "built shared HCOM")
    header = unique((mf / "output").rglob("hcom_service.h"), "installed HCOM header")
    cache_text = (mf / "build/CMakeCache.txt").read_text()
    cache = dict(line.split("=", 1) for line in cache_text.splitlines() if "=" in line and not line.startswith("//"))
    library = cache.get("LIBBOUNDCHECK_SHARED_LIBRARY:FILEPATH", "")
    headers = cache.get("LIBBOUNDCHECK_INCLUDE_DIR:PATH", "")
    fallback = ubs / "dist/hcom_3rdparty/libboundscheck"
    bounds = Path(library) if library and Path(library).is_file() else fallback / "lib/libboundscheck.so"
    bounds_headers = Path(headers) if headers and (Path(headers) / "securec.h").is_file() else fallback / "include"
    if not bounds.is_file() or not (bounds_headers / "securec.h").is_file():
        raise RuntimeError("cannot locate the boundscheck dependency used by the common HCOM build")
    staged_bounds = mf / "output/stage-boundscheck"
    (staged_bounds / "lib").mkdir(parents=True, exist_ok=True)
    (staged_bounds / "include").mkdir(parents=True, exist_ok=True)
    shutil.copy2(bounds, staged_bounds / "lib/libboundscheck.so")
    for header_file in bounds_headers.glob("secure*.h"):
        shutil.copy2(header_file, staged_bounds / "include" / header_file.name)
    include = header.parent.parent
    env = {
        "STAGE_HCOM_INCLUDE": str(include),
        "STAGE_HCOM_LIB": str(archive.parent),
        "STAGE_HCOM_SHARED": str(shared),
        "STAGE_BOUNDSCHECK_ROOT": str(staged_bounds),
    }
    (mf / "output/stage-build.env").write_text("".join(k + "=" + shlex.quote(v) + "\n" for k, v in env.items()))
    files = [
        archive,
        shared,
        bounds,
        mf / "build/CMakeCache.txt",
        mf / "test/indirect_transport_test/hostrdma_batch_bench",
    ]
    manifest = {
        "hcom_source": source_identity(str(ubs)),
        "mf_source": source_identity(str(mf)),
        "sgl_source": source_identity(str(sgl)),
        "paths": env,
        "artifacts": {str(p): digest(p) for p in files},
    }
    if "--final" in sys.argv:
        binary = sgl / "build/rdma_600"
        manifest["artifacts"][str(binary)] = digest(binary)
        cache = (sgl / "build/CMakeCache.txt").read_text()
        if str(archive.parent) not in cache:
            raise RuntimeError("SGL CMake cache does not select the common HCOM directory")
        links = list((sgl / "build").rglob("link.txt"))
        evidence = [p.read_text() for p in links if "rdma_600" in str(p)]
        if not evidence or not any(str(archive) in text for text in evidence):
            raise RuntimeError("SGL link command does not identify the common archive")
        manifest["sgl_link"] = evidence
    (mf / "output/stage-build.json").write_text(json.dumps(manifest, indent=2))
    print("Common HCOM artifact manifest: " + str(mf / "output/stage-build.json"))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError) as error:
        print("ERROR: " + str(error), file=sys.stderr)
        sys.exit(2)
