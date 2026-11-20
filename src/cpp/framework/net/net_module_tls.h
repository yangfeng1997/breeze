// NetModule 的 TLS 装配：把 net.tls 里的路径变成 IStreamLayerFactory，交给 StreamServerOptions::tls / StreamClientOptions::tls。
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

// 这个构建里有没有 TLS（BREEZE_BUILD_NET_TLS）。没有时 tls:// / wss:// 端点要在装配阶段就报错。
bool NetModuleHasTls();

// 失败返回空，error 是原因（文件打不开、证书与私钥不配对、verify_peer 却没有 server_name ……）。
std::shared_ptr<const IStreamLayerFactory> MakeTlsServerLayer(const TlsSetup& setup, std::string& error);
std::shared_ptr<const IStreamLayerFactory> MakeTlsClientLayer(const TlsSetup& setup, std::string& error);

} // namespace breeze
