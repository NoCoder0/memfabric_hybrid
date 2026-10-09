"""Generate MemFabric release notes with GitCode PR attribution and new contributors."""

import argparse
import json
import logging
import os
import re
import subprocess
import sys
import time
from datetime import date, datetime, timedelta, timezone
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.parse import quote, urlencode, urlparse
from urllib.request import Request, urlopen

LOGGER = logging.getLogger(__name__)
API_ROOT = "https://api.gitcode.com/api/v5"
TAG_PATTERN = re.compile(r"^v(\d+)\.(\d+)\.(\d+)(?:([.-][0-9A-Za-z.-]+))?$")
LOGIN_PATTERN = re.compile(r"^[0-9A-Za-z_.-]+$")
PR_PATTERN = re.compile(r"See merge request:\s*([0-9A-Za-z_.-]+/[0-9A-Za-z_.-]+)!(\d+)", re.IGNORECASE)
SECTION_TITLES = {
    "Feature": "Features",
    "Fixed": "Bug Fixes",
    "Performance": "Performance",
    "Changed": "Other Changes",
    "Compatibility": "Compatibility",
    "Documentation": "Documentation",
}
SECTIONS = tuple(SECTION_TITLES)


class ReleaseError(Exception):
    """A release input or API response is unsuitable for publishing."""


def git(*args):
    result = subprocess.run(["git", *args], text=True, capture_output=True, check=False)
    if result.returncode:
        raise ReleaseError(f"git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout.strip()


def version_key(tag):
    match = TAG_PATTERN.fullmatch(tag)
    if not match:
        raise ReleaseError(f"unsupported release tag: {tag}")
    major, minor, patch = map(int, match.group(1, 2, 3))
    suffix = match.group(4)
    return major, minor, patch, 0 if suffix else 1, suffix or ""


def previous_tag(tag):
    current = version_key(tag)
    candidates = []
    for candidate in git("tag", "--list", "v*").splitlines():
        if re.fullmatch(r"v\d+\.\d+\.\d+", candidate) and version_key(candidate) < current:
            candidates.append(candidate)
    if not candidates:
        raise ReleaseError(f"no earlier version tag found for {tag}; pass --base-tag")
    return max(candidates, key=version_key)


def commits_between(base_tag, tag):
    records = git("log", "--first-parent", "--reverse", "--format=%H%x1f%s%x1f%B%x1e", f"{base_tag}..{tag}")
    prs = []
    direct = []
    seen = set()
    for record in records.split("\x1e"):
        fields = record.strip().split("\x1f", 2)
        if len(fields) != 3:
            continue
        sha, subject, body = fields
        matches = [(repo, int(number)) for repo, number in PR_PATTERN.findall(body)]
        if not matches:
            direct.append((sha, subject))
        for key in matches:
            if key not in seen:
                prs.append(key)
                seen.add(key)
    if not prs and not direct:
        raise ReleaseError(f"no commits in {base_tag}..{tag}")
    return prs, direct


def api_json(path, token, method="GET", payload=None, query=None, allow_404=False):
    url = f"{API_ROOT}{path}"
    if query:
        url += "?" + urlencode(query)
    data = None if payload is None else json.dumps(payload, ensure_ascii=False).encode("utf-8")
    request = Request(url, data=data, method=method)
    if token:
        request.add_header("Authorization", f"Bearer {token}")
    request.add_header("Accept", "application/json")
    if data is not None:
        request.add_header("Content-Type", "application/json")
    for attempt in range(3):
        try:
            with urlopen(request, timeout=30) as response:
                return json.load(response)
        except HTTPError as error:
            if allow_404 and error.code == 404:
                return None
            if method == "GET" and error.code in (429, 502, 503) and attempt < 2:
                LOGGER.warning("Retrying GitCode API GET %s after HTTP %s", path, error.code)
                time.sleep(15 * (attempt + 1))
                continue
            raise ReleaseError(f"GitCode API {method} {path} returned HTTP {error.code}") from error
        except (URLError, ValueError) as error:
            raise ReleaseError(f"GitCode API {method} {path} failed: {error}") from error
    raise ReleaseError(f"GitCode API {method} {path} exhausted retries")


def validate_pr(repo, number, pr):
    login = (pr.get("user") or {}).get("login")
    if pr.get("state") != "merged" or not pr.get("merged_at"):
        raise ReleaseError(f"PR !{number} is not merged")
    if not login or not LOGIN_PATTERN.fullmatch(login):
        raise ReleaseError(f"PR !{number} has no usable author login")
    if not pr.get("title"):
        raise ReleaseError(f"PR !{number} has no title")
    try:
        datetime.fromisoformat(pr["merged_at"])
    except ValueError as error:
        raise ReleaseError(f"PR !{number} has an invalid merge time") from error
    link = urlparse(pr.get("html_url") or "")
    if link.scheme != "https" or link.netloc != "gitcode.com" or not link.path.startswith(f"/{repo}/"):
        raise ReleaseError(f"PR !{number} has an invalid GitCode URL")
    if pr.get("number") != number:
        raise ReleaseError(f"PR !{number} returned a different number")
    pr = dict(pr, source_repo=repo)
    return pr


def merged_pr_history(repo, token):
    history = []
    seen = set()
    page = 1
    while True:
        query = {"state": "merged", "page": page, "per_page": 100, "sort": "created", "direction": "asc"}
        batch = api_json(f"/repos/{repo}/pulls", token, query=query)
        if not isinstance(batch, list):
            raise ReleaseError(f"merged PR history for {repo} is not a list (page={page})")
        numbers = {pr["number"] for pr in batch}
        if seen.intersection(numbers):
            raise ReleaseError(f"duplicate PR page in history for {repo} (page={page})")
        seen.update(numbers)
        history.extend(validate_pr(repo, pr["number"], pr) for pr in batch)
        if len(batch) < 100:
            LOGGER.info("Read %d merged PRs from source repository %s", len(history), repo)
            return history
        page += 1


def first_pr_by_author(prs):
    first = {}
    for pr in prs:
        author = (pr["source_repo"], pr["user"]["login"])
        key = (datetime.fromisoformat(pr["merged_at"]), pr["number"])
        previous = first.get(author)
        if previous is None or key < (datetime.fromisoformat(previous["merged_at"]), previous["number"]):
            first[author] = pr
    return first


def collect_prs(keys, token):
    histories = [merged_pr_history(repo, token) for repo in sorted({repo for repo, _ in keys})]
    history = [pr for batch in histories for pr in batch]
    index = {(pr["source_repo"], pr["number"]): pr for pr in history}
    missing = [f"{repo}!{number}" for repo, number in keys if (repo, number) not in index]
    if missing:
        raise ReleaseError(f"PR metadata missing from merged history: {', '.join(missing)}")
    prs = sorted((index[key] for key in keys), key=lambda pr: pr["number"])
    first = first_pr_by_author(history)
    newcomers = [pr for pr in prs if first[(pr["source_repo"], pr["user"]["login"])] is pr]
    return prs, newcomers


def safe_title(title):
    return re.sub(r"\s+", " ", title).replace("[", "\\[").replace("]", "\\]").strip()


def pr_link(pr):
    return f"[!{pr['number']}](https://gitcode.com/{pr['source_repo']}/pull/{pr['number']})"


def bullet(text):
    return f"- {text}"


def classify_pr(pr):
    labels = {label["name"].lower() for label in pr.get("labels", [])}
    for section in SECTIONS:
        if section.lower() in labels:
            return section
    title = pr["title"].lower()
    if re.search(r"breaking|compatibility|兼容|接口.*(?:删减|移除)", title):
        return "Compatibility"
    if re.search(r"fix|bug|修复|解决|崩溃|coredump|报错", title):
        return "Fixed"
    if re.search(r"\bdoc\b|文档|示例|\[examples\]", title):
        return "Documentation"
    if re.search(r"\bperf(?:ormance)?\b|\boptimi[sz]e\b|性能|带宽|吞吐|延迟", title):
        return "Performance"
    if re.search(r"feat|support|支持|新增", title):
        return "Feature"
    return "Changed"


def is_version_only(pr):
    return re.search(r"更新版本号|^bump.*version", pr["title"], re.IGNORECASE) is not None


def change_entries(prs):
    entries = {section: [] for section in SECTIONS}
    for pr in prs:
        if not is_version_only(pr):
            entries[classify_pr(pr)].append(pr)
    return entries


def render_changes(entries):
    if not any(entries.values()):
        return []
    lines = ["", "## 🚀 What's Changed"]
    for section, changes in entries.items():
        if not changes:
            continue
        lines.extend(["", f"### {SECTION_TITLES[section]}", ""])
        for pr in changes:
            lines.append(bullet(f"{safe_title(pr['title'])} by @{pr['user']['login']} in {pr_link(pr)}"))
    return lines


def render_direct_commits(direct, repo):
    if not direct:
        return []
    lines = ["", "## Other Commits", "", "以下提交未确认关联 PR，覆盖范围尚未确认：", ""]
    for sha, subject in direct:
        lines.append(bullet(f"[{sha[:8]}](https://gitcode.com/{repo}/commit/{sha}) — {safe_title(subject)}"))
    return lines


def render_notes(repo, base_tag, tag, prs, direct, newcomers, release_date):
    entries = change_entries(prs)
    lines = [f"# MemFabric Hybrid {tag} - {release_date}", ""]
    if direct:
        lines.extend(["自动生成草稿：存在未确认关联 PR 的提交，覆盖范围尚未确认。", ""])
    lines.extend(
        [
            "## Installation",
            "",
            "- PyPI: [memfabric-hybrid](https://pypi.org/project/memfabric-hybrid/)",
            "- Install: `pip install memfabric-hybrid`",
        ]
    )
    lines.extend(render_changes(entries))
    lines.extend(render_direct_commits(direct, repo))
    if newcomers:
        lines.extend(["", "## 🌱 New Contributors", ""])
        for pr in newcomers:
            lines.append(bullet(f"@{pr['user']['login']} made their first contribution in {pr_link(pr)}"))
    compare = f"https://gitcode.com/{repo}/compare/{quote(base_tag)}...{quote(tag)}"
    lines.extend(["", "## Full Changelog", "", f"[{base_tag}...{tag}]({compare})", ""])
    return "\n".join(lines)


def publish_release(repo, tag, notes, token):
    release_path = f"/repos/{repo}/releases"
    existing = api_json(f"{release_path}/tags/{quote(tag)}", token, allow_404=True)
    if existing is not None:
        LOGGER.info("Release %s already exists for %s; skipping creation", tag, repo)
        return
    status = "pre" if version_key(tag)[3] == 0 else "latest"
    payload = {"tag_name": tag, "name": f"MemFabric Hybrid {tag}", "body": notes, "release_status": status}
    api_json(release_path, token, method="POST", payload=payload)
    LOGGER.info("Created GitCode Release %s for %s", tag, repo)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default="Ascend/memfabric_hybrid")
    parser.add_argument("--tag", required=True)
    parser.add_argument("--base-tag")
    parser.add_argument("--output", default="release-notes.md")
    parser.add_argument("--date", default=datetime.now(timezone(timedelta(hours=8))).date().isoformat())
    parser.add_argument("--publish", action="store_true")
    args = parser.parse_args()
    token = os.environ.get("GITCODE_TOKEN")
    if args.publish and not token:
        raise ReleaseError("GITCODE_TOKEN is required to publish a Release")
    date.fromisoformat(args.date)
    if not re.fullmatch(r"[0-9A-Za-z_.-]+/[0-9A-Za-z_.-]+", args.repo):
        raise ReleaseError(f"invalid repository: {args.repo}")
    version_key(args.tag)
    base_tag = args.base_tag or previous_tag(args.tag)
    if version_key(base_tag) >= version_key(args.tag):
        raise ReleaseError(f"base tag {base_tag} must be earlier than {args.tag}")
    if args.publish and git("rev-parse", "HEAD") != git("rev-parse", f"{args.tag}^{{commit}}"):
        raise ReleaseError(f"checkout HEAD does not match tag {args.tag}")
    LOGGER.info("Using grouped release note format from %s for %s", Path(__file__).resolve(), args.repo)
    keys, direct = commits_between(base_tag, args.tag)
    prs, newcomers = collect_prs(keys, token)
    notes = render_notes(args.repo, base_tag, args.tag, prs, direct, newcomers, args.date)
    Path(args.output).write_text(notes, encoding="utf-8")
    LOGGER.info(
        "Wrote %s: %d PRs, %d direct commits, %d new contributors", args.output, len(prs), len(direct), len(newcomers)
    )
    if args.publish:
        publish_release(args.repo, args.tag, notes, token)


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")
    try:
        main()
    except (ReleaseError, OSError, ValueError, TypeError, KeyError) as error:
        LOGGER.error("GitCode release failed: %s", error)
        sys.exit(1)
