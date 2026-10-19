# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# MindIE is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

import re

ADMONITION_TYPES = {
    "NOTE": "note",
    "TIP": "tip",
    "WARNING": "warning",
    "CAUTION": "danger",
    "IMPORTANT": "important",
}

# Match both `> [!NOTE]` (GitHub) and `>[!NOTE]` (common in copied Obsidian docs)
PATTERN = re.compile(
    r"^([ \t]*)>\s*\[!(" + "|".join(ADMONITION_TYPES.keys()) + r")\](.*?)$\n((?:\1>(?:.*)?$\n)*)",
    re.MULTILINE,
)

# pymdown only parses indented `!!!` when nested under lists/blockquotes with enough indent.
# Few leading spaces (e.g. alignment under **接口格式**) are not real nesting—strip so `!!!` is valid.
MIN_NEST_INDENT_WIDTH = 4


def _leading_space_width(indent: str) -> int:
    return sum(4 if c == "\t" else 1 for c in indent)


def on_page_markdown(markdown, **kwargs):
    def replace_block(match):
        indent = match.group(1)
        gh_type = match.group(2)
        title = match.group(3).strip()
        body_lines = match.group(4)

        nest = indent if _leading_space_width(indent) >= MIN_NEST_INDENT_WIDTH else ""

        mkdocs_type = ADMONITION_TYPES[gh_type]
        if title:
            header = f'{nest}!!! {mkdocs_type} "{title}"'
        else:
            header = f"{nest}!!! {mkdocs_type}"

        converted_lines = []
        for line in body_lines.split("\n"):
            if line.strip() == "":
                converted_lines.append("")
            else:
                content = re.sub(r"^" + re.escape(indent) + r">\s*", "", line)
                converted_lines.append(f"{nest}    {content}")

        body = "\n".join(converted_lines).rstrip()
        return f"{header}\n{body}"

    return PATTERN.sub(replace_block, markdown)
