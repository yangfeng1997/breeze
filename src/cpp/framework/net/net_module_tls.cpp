// NetModule 的 TLS 装配（构建里有 TLS 时链这一份，见 net_module_tls.h）。
#include "framework/net/net_module_tls.h"

#include "base/net/tls/tls_context.h"

namespace breeze
{

namespace
{

std::shared_ptr<const TlsContext> BuildServer(const TlsCredentialsOptions& s, std::string& error)
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

std::shared_ptr<const TlsContext> BuildClient(const TlsCredentialsOptions& s, std::string& error)
{
    TlsClientOptions o;
    o.server_name = s.server_name;
    o.verify_peer = s.verify_peer;
    if (!s.ca_file.empty())
        o.ca.file = s.ca_file;
    o.min_version = s.min_version_13 ? TlsVersion::Tls13 : TlsVersion::Tls12;
    return TlsContext::CreateClient(o, error);
}

// TlsContextHolder 的薄包装：上下文怎么建由 build 决定（服务端 / 客户端），Reload 用同一个 build。
class Reloadable final : public ReloadableTlsLayer
{
public:
    using Build = std::shared_ptr<const TlsContext> (*)(const TlsCredentialsOptions&, std::string&);

    Reloadable(std::shared_ptr<const TlsContext> initial, Build build)
        : holder_(std::move(initial))
        , build_(build)
    {
    }

    std::unique_ptr<IStreamLayer> Create(LayerRole role) const override { return holder_.Create(role); }

    bool Reload(const TlsCredentialsOptions& setup, std::string& error) override
    {
        auto next = build_(setup, error);
        if (!next)
            return false;
        holder_.Reload(std::move(next));
        return true;
    }

private:
    TlsContextHolder holder_;
    Build build_;
};

} // namespace

bool NetModuleHasTls() { return true; }

std::shared_ptr<ReloadableTlsLayer> MakeTlsServerLayer(const TlsCredentialsOptions& s, std::string& error)
{
    auto ctx = BuildServer(s, error);
    return ctx ? std::make_shared<Reloadable>(std::move(ctx), &BuildServer) : nullptr;
}

std::shared_ptr<ReloadableTlsLayer> MakeTlsClientLayer(const TlsCredentialsOptions& s, std::string& error)
{
    auto ctx = BuildClient(s, error);
    return ctx ? std::make_shared<Reloadable>(std::move(ctx), &BuildClient) : nullptr;
}

} // namespace breeze
