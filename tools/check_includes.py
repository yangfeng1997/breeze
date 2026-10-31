#!/usr/bin/env python3
"""检查 C++ 的 include 约定（设计见 docs/网络库设计.md §4.13）：两条规则。

整个仓库的 `#include` 都从 src/cpp 起算（`#include "base/net/endpoint/peer_info.h"`），src/cpp 是唯一的 include 根，
目录不做访问控制，什么头都能 include 到。所以"谁能依赖谁"只能靠这里守着：

一、分层：`base/` 是底层，不能 include `framework/`（见 LAYER_RULES；要加规则就往表里加一行）。
    所有 .h / .cpp 都查，测试也不例外（bench 不在 base/ 与 framework/ 里，不受影响）。

二、net 的公开头：net 之外的非测试代码（framework/ 与其它库）只 include `PUBLIC_HEADERS` 清单里的 net 头，
    include 了清单之外的（EventLoop、UniqueFd、StreamConnection …）就算违规。测试（*_test.cpp）与 bench 不受限：它们要直接戳内部。
    新增公开头时加进 PUBLIC_HEADERS；清单里的路径不存在（改名后忘了改这里）也会报错。
    写成扁平名（`#include "endpoint.h"`）本来就编译不过（没有哪个库再配自己的 include 目录），这里也直接报出来，指向对的写法。

用法：check_includes.py <src/cpp 目录>
有违规则列出位置并以 1 退出；配置不对（找不到 base/net、清单过期、一个文件都没扫到）以 2 退出。只用标准库。
"""

import re
import sys
from pathlib import Path

NET = "base/net/"

# (被约束的目录, 不许 include 的目录, 说明)
LAYER_RULES = [
    ("base/", "framework/", "base 是底层，不能依赖 framework"),
]

PUBLIC_HEADERS = {
    # connection/：连接、回调、句柄、选项、服务端 / 客户端接口
    NET + "connection/connection_interface.h",
    NET + "connection/connection_context.h",
    NET + "connection/connection_handle.h",
    NET + "connection/connection_handler_interface.h",
    NET + "connection/connection_options.h",
    NET + "connection/net_client_interface.h",
    NET + "connection/net_server_interface.h",
    NET + "connection/send_result.h",
    # endpoint/、codec/
    NET + "endpoint/endpoint.h",
    NET + "endpoint/peer_info.h",
    NET + "codec/frame_codec_interface.h",
    NET + "codec/length_prefix_codec.h",
    # loop/：EventLoopGroup 与统计（EventLoop 本身不是公开类型）
    NET + "loop/event_loop_group.h",
    NET + "loop/net_stats.h",
    # stream/：TCP / UDS 共用的服务端 / 客户端基类
    NET + "stream/reconnect_policy.h",
    NET + "stream/stream_client.h",
    NET + "stream/stream_server.h",
    # tcp/、uds/、rudp/：端点、选项、服务端、客户端
    NET + "tcp/tcp_client.h",
    NET + "tcp/tcp_endpoint.h",
    NET + "tcp/tcp_options.h",
    NET + "tcp/tcp_server.h",
    NET + "uds/uds_client.h",
    NET + "uds/uds_endpoint.h",
    NET + "uds/uds_server.h",
    NET + "rudp/rudp_client.h",
    NET + "rudp/rudp_endpoint.h",
    NET + "rudp/rudp_options.h",
    NET + "rudp/rudp_protocol.h",
    NET + "rudp/rudp_server.h",
    NET + "rudp/rudp_ticket.h",
}

INCLUDE = re.compile(r'^\s*#\s*include\s+"([A-Za-z0-9_./]+\.h)"', re.M)
SKIP_DIRS = {"gen", "build", ".git"}


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    root = Path(sys.argv[1])
    net = root / "base" / "net"
    if not net.is_dir():
        print(f"找不到 {net}", file=sys.stderr)
        return 2

    net_headers = {p.relative_to(root).as_posix() for p in net.rglob("*.h")}
    net_by_name = {Path(h).name: h for h in net_headers}
    stale = sorted(PUBLIC_HEADERS - net_headers)
    if stale:
        print("PUBLIC_HEADERS 里这些头在 base/net 下找不到（改名了？）：" + ", ".join(stale), file=sys.stderr)
        return 2

    scanned = 0
    layer_violations = []
    net_violations = []
    for p in sorted(root.rglob("*")):
        if p.suffix not in (".h", ".cpp") or any(part in SKIP_DIRS for part in p.relative_to(root).parts):
            continue
        rel = p.relative_to(root)
        rel_posix = rel.as_posix()
        scanned += 1
        text = p.read_text(encoding="utf-8", errors="replace")
        in_net = net in p.parents
        net_exempt = in_net or rel.parts[0] == "bench" or p.name.endswith("_test.cpp")  # net 自己、bench、测试不受限
        for m in INCLUDE.finditer(text):
            name = m.group(1)
            line = text.count("\n", 0, m.start()) + 1
            for src, forbidden, why in LAYER_RULES:
                if rel_posix.startswith(src) and name.startswith(forbidden):
                    layer_violations.append(f"{rel}:{line}: include \"{name}\"（{why}）")
            if net_exempt:
                continue
            if name in net_headers:
                if name not in PUBLIC_HEADERS:
                    net_violations.append(f"{rel}:{line}: include \"{name}\"（net 的非公开头）")
            elif "/" not in name and name in net_by_name:
                # 扁平名正好是 net 的某个头：没有哪个库配 include 目录了，要写成从 src/cpp 起算的路径
                net_violations.append(f"{rel}:{line}: include \"{name}\" 要写成 \"{net_by_name[name]}\"")

    if scanned == 0:
        print("一个文件都没扫到：路径写错了？", file=sys.stderr)
        return 2
    failed = False
    if layer_violations:
        failed = True
        print("违反分层的 include：", file=sys.stderr)
        for v in layer_violations:
            print("  " + v, file=sys.stderr)
    if net_violations:
        failed = True
        print("net 之外的代码 include 了非公开头，或没有从 src/cpp 起算：", file=sys.stderr)
        for v in net_violations:
            print("  " + v, file=sys.stderr)
    if failed:
        return 1
    print(f"check_includes: {scanned} 个文件，没有违规")
    return 0


if __name__ == "__main__":
    sys.exit(main())
