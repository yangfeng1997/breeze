# Rust 环境

使用最新稳定版 Rust（`rust-toolchain.toml` 的 `stable` 通道），Edition 2024。
2026-11-24 配置时的稳定版是 Rust 1.100.0；本项目最低版本为 1.100。

## 依赖

- `redis` 1.8.3：redis-rs，开启 `cluster-async` 与 `tokio-comp`，使用 Tokio 异步 Redis Cluster。
- `etcd-client` 0.21.1：etcd v3 客户端，构建时需要 `protoc`。
- `tokio` 1.54.1：开启多线程异步运行时与宏。

版本下限写在 `Cargo.toml`，实际解析版本由 `Cargo.lock` 锁定；锁文件应进版本库。
以上客户端未开启可选 TLS 功能；后续使用 `rediss://` 或 HTTPS etcd 时再配置 TLS。
`src/main.rs` 仅保留空入口，不连接服务、不读写数据。

## Windows

安装官网的 [rustup-init.exe](https://rust-lang.org/zh-CN/learn/get-started/)，
使用默认的 `x86_64-pc-windows-msvc` 工具链。
以后编译需要 Visual Studio Build Tools 的“使用 C++ 的桌面开发”工作负载与 Windows SDK。

`protoc` 需要在 PATH 中；若报找不到 `google/protobuf/*.proto`，
将 `PROTOC_INCLUDE` 指向 Protobuf 安装目录的 `include` 目录。

## 常用命令

在 `src/rust` 下执行：

```powershell
rustc --version
cargo --version
rustup update stable
cargo fetch --locked
```

`cargo fetch` 只下载依赖，不编译。后续需要构建时使用 `cargo check --locked` 或 `cargo build --locked`。
更新兼容版本的依赖使用 `cargo update`；跨不兼容版本升级需要同时修改 `Cargo.toml`。
