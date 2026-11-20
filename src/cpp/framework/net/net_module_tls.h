// NetModule 的 TLS 装配：把 net.tls 里的路径变成可热更的 IStreamLayerFactory，交给 StreamServerOptions::tls /
// StreamClientOptions::tls。
//
// **链接期选择**：BREEZE_BUILD_NET_TLS=ON 时 breeze_net_module 链 net_module_tls.cpp（调 breeze_net_tls），OFF 时链
// net_module_tls_stub.cpp（明确报错，不退化成明文）。所以本头只出现 IStreamLayerFactory，不含任何 TLS 类型，
// framework/net 的其它代码也不依赖 BoringSSL。
//
// 证书与私钥都给**文件路径**（PEM，叶证书在前）；密钥内容不进配置、不进日志。

#pragma once

#include <memory>
#include <string>

#include "base/net/stream/stream_layer_interface.h"

namespace breeze
{

// 对应 config/schema/net.proto 的 TlsConfig（NetModule 负责搬运，不做别的解释）。
struct TlsSetup
{
    std::string cert_chain_file;
    std::string private_key_file;
    std::string ca_file;
    std::string server_name;
    bool verify_peer = true;
    bool require_client_cert = false;
    bool min_version_13 = false;
};

// 可热更的 TLS 层工厂：Reload 用 setup 里的文件重建上下文并原子替换；之后**新建**的连接用新上下文，已建立的连接不受影响。
// 重建失败（文件读不出来、证书与私钥对不上 ……）返回 false、error 是原因，**旧上下文原样保留**。任意线程可调。
class ReloadableTlsLayer : public IStreamLayerFactory
{
public:
    virtual bool Reload(const TlsSetup& setup, std::string& error) = 0;
};

// 这个构建里有没有 TLS（BREEZE_BUILD_NET_TLS）。没有时 tls:// / wss:// 端点要在装配阶段就报错。
bool NetModuleHasTls();

// 失败返回空，error 是原因（文件打不开、证书与私钥不配对、verify_peer 却没有 server_name ……）。
std::shared_ptr<ReloadableTlsLayer> MakeTlsServerLayer(const TlsSetup& setup, std::string& error);
std::shared_ptr<ReloadableTlsLayer> MakeTlsClientLayer(const TlsSetup& setup, std::string& error);

} // namespace breeze
