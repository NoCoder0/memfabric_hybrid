load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")
load("@bazel_tools//tools/build_defs/repo:utils.bzl", "maybe")
load("//bazel:version_repo.bzl", "version_repo")

def memfabric_deps():
    maybe(
        git_repository,
        name = "libboundscheck",
        remote = "https://atomgit.com/openeuler/libboundscheck.git",
        branch = "master",
        build_file = "@hcom//src/ubsocket/3rdparty/boundscheck:BUILD.bazel",
    )
    maybe(
        git_repository,
        name = "hcom",
        remote = "https://github.com/NoCoder0/ubs-comm.git",
        commit = "740f0bbb25134eeb63f5061f019f0497e6b0effa",
    )

    version_repo(
        name = "version_info",
        build_from_memcache = "false",
    )
