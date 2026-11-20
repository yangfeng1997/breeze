#include "base/net/tls/internal/tls_bio.h"

#include <openssl/bio.h>

#include <algorithm>
#include <cstring>

namespace breeze::tls
{

namespace
{

int BioCreate(BIO* bio)
{
    BIO_set_init(bio, 1);
    return 1;
}

int BioDestroy(BIO*) { return 1; } // state 归 TlsLayer 所有

int BioRead(BIO* bio, char* buf, int len)
{
    auto* st = static_cast<TlsBioState*>(BIO_get_data(bio));
    BIO_clear_retry_flags(bio);
    if (st->in.empty())
    {
        BIO_set_retry_read(bio); // 这一批线上字节吃完了：SSL 返回 WANT_READ，等下一次 Unwrap
        return -1;
    }
    const std::size_t n = std::min(static_cast<std::size_t>(len), st->in.size());
    std::memcpy(buf, st->in.data(), n);
    st->in = st->in.subspan(n);
    return static_cast<int>(n);
}

int BioWrite(BIO* bio, const char* data, int len)
{
    auto* st = static_cast<TlsBioState*>(BIO_get_data(bio));
    BIO_clear_retry_flags(bio);
    st->out->Append({reinterpret_cast<const std::byte*>(data), static_cast<std::size_t>(len)});
    return len;
}

long BioCtrl(BIO*, int cmd, long, void*)
{
    switch (cmd)
    {
        case BIO_CTRL_FLUSH:
            return 1; // 没有下游缓冲要刷
        default:
            return 0;
    }
}

const BIO_METHOD* Method()
{
    // 进程内只建一次（C++ 的局部静态初始化线程安全），不释放。
    static const BIO_METHOD* const method = []
    {
        BIO_METHOD* m = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "breeze-tls");
        if (m != nullptr)
        {
            BIO_meth_set_create(m, BioCreate);
            BIO_meth_set_destroy(m, BioDestroy);
            BIO_meth_set_read(m, BioRead);
            BIO_meth_set_write(m, BioWrite);
            BIO_meth_set_ctrl(m, BioCtrl);
        }
        return m;
    }();
    return method;
}

} // namespace

bio_st* NewBio(TlsBioState* state)
{
    const BIO_METHOD* m = Method();
    if (m == nullptr)
        return nullptr;
    BIO* bio = BIO_new(m);
    if (bio == nullptr)
        return nullptr;
    BIO_set_data(bio, state);
    return bio;
}

} // namespace breeze::tls
