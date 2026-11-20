// NetModule 的 TLS 装配的桩（BREEZE_BUILD_NET_TLS=OFF 时链这一份）：配置里要 TLS 就直接报错，
// 不静默退化成明文——加密被悄悄关掉比启动失败严重得多。
#include "framework/net/net_module_tls.h"

namespace breeze
{

bool NetModuleHasTls() { return false; }

std::shared_ptr<ReloadableTlsLayer> MakeTlsServerLayer(const TlsSetup&, std::string& error)
{
    error = "这个构建没有 TLS（BREEZE_BUILD_NET_TLS=OFF），不能监听 tls:// / wss://";
    return nullptr;
}

std::shared_ptr<ReloadableTlsLayer> MakeTlsClientLayer(const TlsSetup&, std::string& error)
{
    error = "这个构建没有 TLS（BREEZE_BUILD_NET_TLS=OFF），不能连接 tls:// / wss://";
    return nullptr;
}

} // namespace breeze
