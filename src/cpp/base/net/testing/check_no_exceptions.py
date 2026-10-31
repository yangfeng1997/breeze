#!/usr/bin/env python3
"""检查 net 库源码里没有 throw / try / catch（设计见 docs/网络库设计.md §4.11）。

用法：check_no_exceptions.py <目录>...
先剥掉注释与字符串/字符字面量再查关键字：注释和日志文案里出现这些词不算违规。
不存在的目录跳过（framework/net 还没有时也能跑）；一个文件都没扫到视为配置错误，以 2 退出，避免路径写错时静默通过。
有违规则列出位置并以 1 退出。只用标准库。
"""

import re
import sys
from pathlib import Path

EXTS = {".h", ".hpp", ".cpp", ".cc"}
KEYWORD = re.compile(r"\b(throw|try|catch)\b")

# 按出现顺序匹配，先到先得：字符串里的 "//" 不会被当成注释，注释里的引号也不会被当成字符串。
STRIP = re.compile(
    r"""
      //[^\n]*                                   # 行注释
    | /\*.*?\*/                                  # 块注释
    | R"(?P<d>[^()\\\s]{0,16})\(.*?\)(?P=d)"     # 原始字符串
    | "(?:\\.|[^"\\\n])*"                        # 字符串
    | '(?:\\.|[^'\\\n])*'                        # 字符
    """,
    re.S | re.X,
)


def strip(src: str) -> str:
    # 被剥掉的部分换成等量换行，行号不变。
    return STRIP.sub(lambda m: "\n" * m.group(0).count("\n"), src)


def main(argv: list[str]) -> int:
    scanned = 0
    bad: list[str] = []
    for d in argv[1:]:
        root = Path(d)
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*")):
            if path.suffix not in EXTS or not path.is_file():
                continue
            scanned += 1
            for no, line in enumerate(strip(path.read_text(encoding="utf-8")).splitlines(), 1):
                m = KEYWORD.search(line)
                if m:
                    bad.append(f"{path}:{no}: 出现 {m.group(1)}")
    if scanned == 0:
        print("check_no_exceptions: 没有扫到任何源文件，检查目录参数", file=sys.stderr)
        return 2
    for b in bad:
        print(b, file=sys.stderr)
    print(f"check_no_exceptions: 扫描 {scanned} 个文件，违规 {len(bad)} 处")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
