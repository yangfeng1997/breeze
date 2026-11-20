// NetModule 的 TLS 装配（构建里有 TLS 时链这一份，见 net_module_tls.h）。
#include "framework/net/net_module_tls.h"

#include "base/net/tls/tls_context.h"

namespace breeze
{

bool NetModuleHasTls() { return true; }

std::shared_ptr<const IStreamLayerFactory> MakeTlsServerLayer(const TlsSetup& s, std::string& error)
{
    TlsServerOptions o;
    o.cert_chain.file = s.cert_chain_file;
    o.private_key.file = s.private_key_file;
    if (!s.ca_file.empty())
        o.client_ca.file = s.ca_file;
    o.require_client_cert = s.require_client_cert;
    o.min_version = s.min_version_13 ? TlsVersion::Tls13 : TlsVersion::Tls12;
    return TlsContext::CreateServer(o, error);
}

std::shared_ptr<const IStreamLayerFactory> MakeTlsClientLayer(const TlsSetup& s, std::string& error)
{
    TlsClientOptions o;
    o.server_name = s.server_name;
    o.verify_peer = s.verify_peer;
    if (!s.ca_file.empty())
        o.ca.file = s.ca_file;
    o.min_version = s.min_version_13 ? TlsVersion::Tls13 : TlsVersion::Tls12;
    return TlsContext::CreateClient(o, error);
}

} // namespace breeze
