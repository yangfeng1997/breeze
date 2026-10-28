#!/usr/bin/env python3
"""生成全部代码，然后配置 C++ 构建。

    python3 scripts/configure.py                 # 生成 + cmake --preset debug
    python3 scripts/configure.py --preset release
    python3 scripts/configure.py --gen-only      # 只生成，不跑 cmake

生成物（都在 .gitignore 里，不进版本库）：
    src/cpp/gen/config/       config/schema/*.proto → C++ 头文件
    src/go/gen/config/        同上 → Go 源文件
    build/jsonschema/         同上 → JSON Schema（编辑器里给 yaml 补全）
新增一类生成物（如策划表）：在 GENERATORS 里加一项，输出放 src/<语言>/gen/<类别>/。

C++ 构建本身也会在 schema 变动时重新生成 src/cpp/gen/config（见 src/cpp/CMakeLists.txt），
本脚本的价值是一次性把 Go / JSON Schema 等其它语言的产物也带上，并清掉过期文件。
只用标准库，不依赖 pip。
"""

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CONFIGGEN = ROOT / "tools" / "configgen" / "configgen.py"


def gen_config(protoc: "str | None") -> list:
    cmd = [
        sys.executable, str(CONFIGGEN),
        "--schema-dir", str(ROOT / "config" / "schema"),
        "--cpp-out", str(ROOT / "src" / "cpp" / "gen" / "config"),
        "--go-out", str(ROOT / "src" / "go" / "gen" / "config"),
        "--jsonschema-out", str(ROOT / "build" / "jsonschema"),
        "--prune", "-v",
    ]
    if protoc:
        cmd += ["--protoc", protoc]
    return cmd


# (名字, 构造命令的函数)；按顺序执行，任何一步失败就停。
GENERATORS = [
    ("config", gen_config),
]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--preset", default="debug", help="cmake preset，默认 debug")
    ap.add_argument("--gen-only", action="store_true", help="只生成代码，不配置 cmake")
    ap.add_argument("--protoc", help="protoc 路径；默认取环境变量 PROTOC，再找 PATH")
    args = ap.parse_args()

    for name, build_cmd in GENERATORS:
        print(f"==> 生成 {name}")
        if subprocess.run(build_cmd(args.protoc)).returncode != 0:
            print(f"configure: 生成 {name} 失败", file=sys.stderr)
            return 1

    if args.gen_only:
        return 0
    print(f"==> cmake --preset {args.preset}")
    return subprocess.run(["cmake", "--preset", args.preset], cwd=ROOT / "src" / "cpp").returncode


if __name__ == "__main__":
    sys.exit(main())
