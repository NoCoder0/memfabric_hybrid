#!/usr/bin/env python3
# SPDX-License-Identifier: MulanPSL-2.0
"""Compile production benchmark helpers on CPU; no transport/device performance claim."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path


def body(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class SourceUpdateTests(unittest.TestCase):
    def test_parser_markers_and_full_verification(self):
        bench = Path(__file__).with_name("hostrdma_batch_bench.cpp").read_text(encoding="utf-8")
        source = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
namespace mf_trace { void Mark(const char *) {} }
'''
        source += bench[bench.index("constexpr uint32_t kDefaultCount"):bench.index("void Usage(")]
        source += "constexpr uint32_t kBlockIdBytes = 4;\n"
        for name in ("bool ParseArgs(", "uint32_t BlockTag(", "uint8_t BlockFillByte(", "void FillBlock(",
                     "void FillMarkers(", "void PrepareSource(", "struct VerifyResult {",
                     "VerifyResult VerifyMarkerBlocks(", "VerifyResult VerifyBlocks("):
            source += body(bench, name) + (";\n" if name.startswith("struct") else "\n")
        source += r'''
int main() {
    BenchArgs defaults;
    assert(defaults.sourceUpdate == "static");
    for (auto mode : {"static", "markers", "bad"}) {
        BenchArgs a;
        std::vector<std::string> args{"bench", "--role=remote", "--store-url=tcp://x:1",
                                      "--hcom-url=tcp://x:2", std::string("--source-update=") + mode};
        std::vector<char *> argv;
        for (auto &arg : args) argv.push_back(&arg[0]);
        assert(ParseArgs(argv.size(), argv.data(), a) == (std::string(mode) != "bad"));
    }
    for (auto mode : {"static", "markers"}) for (uint64_t size : {656, 1024}) {
        BenchArgs a;
        a.count = 3; a.size = size; a.sourceUpdate = mode;
        std::vector<uint8_t> storage(3 * 4096, 0x5a), refs(256 * size);
        std::vector<void *> srcs;
        for (uint32_t v = 0; v < 256; ++v) memset(refs.data() + v * size, v, size);
        for (uint32_t i = 0; i < a.count; ++i) {
            srcs.push_back(storage.data() + i * 4096);
            FillBlock(srcs.back(), i, size);
        }
        const auto original = storage;
        for (uint64_t generation : {1, 2, 121, 122}) {
            PrepareSource(a, srcs, generation);
            const auto expected = a.sourceUpdate == "markers" ? generation : 0;
            assert(VerifyBlocks(srcs, a.count, size, refs.data(), expected).ok);
            if (expected) assert(!VerifyBlocks(srcs, a.count, size, refs.data(), generation + 1).ok);
            else assert(storage == original);
            for (uint32_t i = 0; i < a.count; ++i) {
                assert(memcmp(storage.data() + i * 4096 + 8, original.data() + i * 4096 + 8, size - 16) == 0);
                assert(memcmp(storage.data() + i * 4096 + size, original.data() + i * 4096 + size, 4096 - size) == 0);
            }
            for (uint64_t offset : {uint64_t(0), uint64_t(100), size - 1}) {
                storage[offset] ^= 1;
                assert(!VerifyBlocks(srcs, a.count, size, refs.data(), expected).ok);
                storage[offset] ^= 1;
            }
            std::swap(srcs[0], srcs[1]);
            assert(!VerifyBlocks(srcs, a.count, size, refs.data(), expected).ok);
            std::swap(srcs[0], srcs[1]);
        }
    }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "source.cpp"
            exe = Path(directory) / "source.exe"
            cpp.write_text(source, encoding="utf-8")
            result = subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                     str(cpp), "-o", str(exe)], capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(exe)], check=True, timeout=10, capture_output=True)


if __name__ == "__main__":
    unittest.main()
