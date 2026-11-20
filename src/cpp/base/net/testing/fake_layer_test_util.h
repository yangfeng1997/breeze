// 假的字节流层（只给测试）：握手 = 客户端发 "HELO"、服务端回 "OK!!"；之后每个字节与 0x5A 异或（所以线上字节一眼能认出来，
// 也证明读写真的经过了层）。用来在不依赖 BoringSSL 的情况下测分层连接：握手中途断开 / 超时 / 失败、握手字节被任意切分、
// 握手完成的同一次读里就带着明文、层出错、优雅关闭时的 Shutdown、大 payload 跨预算、积压与保序。

#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "base/buffer/buffer.h"
#include "base/net/stream/stream_layer_interface.h"

namespace breeze::testing
{

enum class FakeLayerMode
{
    Normal,
    NeverDone,     // 吞掉握手字节、从不完成
    FailHandshake, // 服务端收到完整的 "HELO" 后报 Error
    FailAfter,     // 握手完成后，累计交付了 fail_after 个明文字节就报 Error
};

// 工厂与各个层共享的计数，测试据此断言层被创建 / 启动 / 关闭 / 销毁了几次。
struct FakeLayerCounters
{
    std::atomic<int> created{0};
    std::atomic<int> started{0};
    std::atomic<int> shutdowns{0};
    std::atomic<int> destroyed{0};
};

inline constexpr std::byte kFakeLayerKey{0x5A};

inline std::string FakeXor(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(static_cast<std::byte>(c) ^ kFakeLayerKey);
    return s;
}

class FakeLayer final : public IStreamLayer
{
public:
    FakeLayer(LayerRole role, FakeLayerMode mode, FakeLayerCounters* counters, std::size_t fail_after)
        : client_(role == LayerRole::Client)
        , mode_(mode)
        , counters_(counters)
        , fail_after_(fail_after)
    {
        ++counters_->created;
    }
    ~FakeLayer() override { ++counters_->destroyed; }

    LayerStatus Start() override
    {
        ++counters_->started;
        if (client_)
            Emit("HELO");
        return LayerStatus::Ok;
    }

    LayerStatus Unwrap(std::span<std::byte> wire, PlainSink sink) override
    {
        if (mode_ == FakeLayerMode::NeverDone)
            return LayerStatus::Ok;
        if (!ready_)
        {
            std::size_t i = 0;
            while (i < wire.size() && hs_.size() < 4)
                hs_.push_back(static_cast<char>(wire[i++]));
            if (hs_.size() < 4)
                return LayerStatus::Ok; // 握手字节还没到齐：这一批已经吃完了
            if (client_)
            {
                if (hs_ != "OK!!")
                    return Fail();
            }
            else
            {
                if (hs_ != "HELO" || mode_ == FakeLayerMode::FailHandshake)
                    return Fail();
                Emit("OK!!");
            }
            ready_ = true;
            const LayerStatus st = Deliver(wire.subspan(i), sink); // 握手完成的同一次读里就可能带着明文
            return st == LayerStatus::Ok ? LayerStatus::HandshakeDone : st;
        }
        return Deliver(wire, sink);
    }

    LayerStatus Wrap(std::span<const std::byte> plain) override
    {
        const std::size_t before = wire_.ReadableBytes();
        wire_.Append(plain);
        for (std::byte& b : wire_.ReadableMut().subspan(before))
            b ^= kFakeLayerKey;
        return LayerStatus::Ok;
    }

    std::span<const std::byte> PendingWire() const noexcept override { return wire_.Readable(); }
    void ConsumeWire(std::size_t n) noexcept override { wire_.Consume(n); }

    LayerStatus Shutdown() override
    {
        ++counters_->shutdowns;
        return LayerStatus::Ok;
    }

    int error_code() const noexcept override { return 4242; }

private:
    void Emit(const char* s)
    {
        const std::string str(s);
        wire_.Append({reinterpret_cast<const std::byte*>(str.data()), str.size()}); // 握手字节不异或
    }
    LayerStatus Fail()
    {
        failed_ = true;
        return LayerStatus::Error;
    }
    LayerStatus Deliver(std::span<std::byte> bytes, PlainSink sink)
    {
        if (bytes.empty())
            return LayerStatus::Ok;
        for (std::byte& b : bytes)
            b ^= kFakeLayerKey;
        delivered_ += bytes.size();
        (void)sink(bytes);
        if (mode_ == FakeLayerMode::FailAfter && delivered_ >= fail_after_)
            return Fail();
        return LayerStatus::Ok;
    }

    const bool client_;
    const FakeLayerMode mode_;
    FakeLayerCounters* const counters_;
    const std::size_t fail_after_;
    bool ready_ = false;
    bool failed_ = false;
    std::string hs_;
    std::size_t delivered_ = 0;
    Buffer wire_;
};

class FakeLayerFactory final : public IStreamLayerFactory
{
public:
    explicit FakeLayerFactory(FakeLayerMode mode = FakeLayerMode::Normal, std::size_t fail_after = 0)
        : mode_(mode)
        , fail_after_(fail_after)
    {
    }

    std::unique_ptr<IStreamLayer> Create(LayerRole role) const override
    {
        return std::make_unique<FakeLayer>(role, mode_, &counters, fail_after_);
    }

    mutable FakeLayerCounters counters;

private:
    FakeLayerMode mode_;
    std::size_t fail_after_;
};

} // namespace breeze::testing
