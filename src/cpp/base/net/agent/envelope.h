// Agent 转发用的消息信封：每条消息（共享内存环里的一条记录、UDS 上的一帧、Agent 之间 TCP 上的一帧）的载荷都以它开头，
// 后面紧跟业务载荷。设计与调研依据见 docs/共享内存IPC设计.md §4 与 §10（参考了 brpc / Tars / gRPC / Thrift 与两个自有项目的做法）。
//
// 几条硬规矩（都来自调研里别人踩过的坑）：
// - **路由区定长、固定偏移、纯数字**：前 48 字节装下转发需要的全部字段（类型、目的、路由类型、路由键、剩余毫秒……），
//   Agent 转发一条消息只碰记录的第一个缓存行（16 字节记录头 + 这 48 字节）；没有字符串、没有变长整数，改写 remaining_ms 不会改变长度。
// - **魔数 + 版本 + 精确的长度**：body_len 必须恰好等于 载荷长度 - 64，多一个字节少一个字节都丢；保留位必须为 0；不认识的一律丢弃并计数，不退化到别的路径。
// - **单向用显式的类型**（Oneway），不靠 seq == 0 的约定；**Agent 不改写 seq**：回包按 (src, seq) 索引，seq 的命名空间是发送方。
// - **剩余时间是相对毫秒**，转发时用记录头里的入队时刻把排队时间扣掉再写进拷贝（见 CheckExpire / PatchForForward）。
// - **发送方的身份要核对**：本机 App 的 src 必须等于握手时登记的 NodeId（AgentRouter 做），否则可以冒充别人。
//
// 小端；同机、同架构的进程之间直接收发。Agent 之间走 TCP 也假定两端同为小端（x86 / ARM64）。

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace breeze::agent
{

static_assert(std::endian::native == std::endian::little, "信封按小端直接收发");

inline constexpr std::uint32_t kEnvelopeMagic = 0x564E4542; // 字节序列 'B' 'E' 'N' 'V'
inline constexpr std::uint8_t kEnvelopeVersion = 1;
inline constexpr std::size_t kEnvelopeHeadSize = 64;
inline constexpr std::size_t kEnvelopeRoutingBytes = 48; // 路由决策只读这么多
inline constexpr std::uint8_t kMaxHops = 8;              // 转发次数上限，防环

// 进程 / 服务实例的编号。库里只当不透明的 u32（自有项目里是 world16 | serverType8 | index8）；0 = 未指定。
using NodeId = std::uint32_t;

enum class EnvType : std::uint8_t
{
    kRequest = 1,  // 要回包
    kResponse = 2, // 回包：route_type 必须是 Direct，dst 是请求的 src；err_code 非 0 表示失败
    kOneway = 3,   // 单向：不关心结果，可以丢（设计 §4）
};

enum class RouteType : std::uint8_t
{
    kDirect = 1,    // 发给 dst
    kAny = 2,       // service_type 的任意一个实例
    kHash = 3,      // service_type 的实例里按 route_key 选一个
    kBroadcast = 4, // service_type 的全部实例
};

// flags 位
inline constexpr std::uint16_t kEnvFlagTrace = 1u << 0; // 追踪（染色）：下游沿调用链带着
inline constexpr std::uint16_t kEnvKnownFlags = kEnvFlagTrace;

// 64 字节，小端，字段偏移由 static_assert 锁死。前 48 字节是路由区。
struct EnvelopeHead
{
    std::uint32_t magic;        // 0
    std::uint8_t version;       // 4
    std::uint8_t type;          // 5   EnvType
    std::uint16_t flags;        // 6
    std::uint32_t body_len;     // 8   头之后的载荷字节数；必须恰好等于 总长 - 64
    std::uint32_t msg_id;       // 12  业务消息号
    NodeId src;                 // 16  发送方
    NodeId dst;                 // 20  Direct / Response 必填
    std::uint16_t service_type; // 24  Any / Hash / Broadcast 的目的服务类型
    std::uint8_t route_type;    // 26  RouteType
    std::uint8_t hops;          // 27  已经转发的次数，每一跳加一
    std::uint8_t gray_tag;      // 28  灰度标记（Agent 透传，路由表可以据此选实例）
    std::uint8_t reserved0[3];  // 29  必须为 0
    std::uint64_t route_key;    // 32  Hash 用的数值（实体 id 或发送方算好的哈希）
    std::uint32_t remaining_ms; // 40  发送那一刻还剩的毫秒数；0 无效（设计 §4）
    std::uint32_t err_code;     // 44  只有 Response 用
    // ---- 以上是路由区（48 字节）；以下只在回包与追踪时才用，Agent 转发不读 ----
    std::uint64_t seq;      // 48  相关性：单调递增、永不复用、命名空间是发送方
    std::uint64_t trace_id; // 56
};

static_assert(sizeof(EnvelopeHead) == kEnvelopeHeadSize);
static_assert(std::is_trivially_copyable_v<EnvelopeHead>);
static_assert(offsetof(EnvelopeHead, body_len) == 8 && offsetof(EnvelopeHead, src) == 16 &&
              offsetof(EnvelopeHead, service_type) == 24 && offsetof(EnvelopeHead, route_key) == 32 &&
              offsetof(EnvelopeHead, remaining_ms) == 40 && offsetof(EnvelopeHead, err_code) == 44 &&
              offsetof(EnvelopeHead, seq) == 48 && offsetof(EnvelopeHead, trace_id) == 56);
// 路由区之内的字段必须都在前 48 字节里。
static_assert(offsetof(EnvelopeHead, err_code) + sizeof(EnvelopeHead::err_code) == kEnvelopeRoutingBytes);

enum class EnvStatus : std::uint8_t
{
    kOk,
    kTooShort,   // 不足 64 字节
    kBadMagic,
    kBadVersion,
    kBadType,
    kBadRoute,   // route_type 不认识，或 Direct / Response 没有 dst，或 Response 不是 Direct
    kBadFlags,   // 有不认识的位
    kBadReserved, // 保留位非零
    kBadLength,  // body_len 与总长对不上
};

constexpr const char* ToString(EnvStatus s) noexcept
{
    switch (s)
    {
        case EnvStatus::kOk:
            return "Ok";
        case EnvStatus::kTooShort:
            return "TooShort";
        case EnvStatus::kBadMagic:
            return "BadMagic";
        case EnvStatus::kBadVersion:
            return "BadVersion";
        case EnvStatus::kBadType:
            return "BadType";
        case EnvStatus::kBadRoute:
            return "BadRoute";
        case EnvStatus::kBadFlags:
            return "BadFlags";
        case EnvStatus::kBadReserved:
            return "BadReserved";
        case EnvStatus::kBadLength:
            return "BadLength";
    }
    return "?";
}

// 解析并校验一条消息的信封：先把 64 字节拷到本地 out 再校验（载荷在共享内存里，对端随时可能改，不能对它多次读取）。
// bytes 是信封 + 业务载荷的整段。成功时 out 可信；失败时 out 内容不确定。
EnvStatus ParseEnvelope(std::span<const std::byte> bytes, EnvelopeHead& out) noexcept;

// 把头写进 dst 的前 64 字节（dst 至少 64 字节）。
void WriteEnvelopeHead(std::span<std::byte> dst, const EnvelopeHead& head) noexcept;

enum class ExpireStatus : std::uint8_t
{
    kAlive,
    kExpired, // 剩余不足 1 毫秒也算过期：写进去的 remaining_ms 不能是 0
    kInvalid, // remaining_ms 或入队时刻是 0：按已过期处理，单独计数（设计 §4）
};

// 现在还剩多少：remaining_ms × 10^6 - (now_ns - enqueue_ts_ns)，向下取整到毫秒。
// enqueue_ts_ns 是通道在 Commit 时盖的单调时刻（比真实入队略早，所以只会早丢、不会晚丢）；now_ns 是同一个时钟（Now::SteadyNs）。
// 不是从环里来的消息（UDS / TCP 的帧）没有入队时刻，调用方传"收到的时刻"，排队时间记 0。
// left_ms 只在 kAlive 时有意义，且至少是 1。now_ns 比入队时刻还早（不该发生）按排队时间 0 算。
ExpireStatus CheckExpire(std::uint32_t remaining_ms,
                         std::uint64_t enqueue_ts_ns,
                         std::uint64_t now_ns,
                         std::uint32_t* left_ms) noexcept;

// 转发之前在拷贝里改：剩余毫秒换成扣过排队时间的值，hops 加一。成功返回 true；hops 已到 kMaxHops 返回 false（头不变）。
bool PatchForForward(EnvelopeHead& head, std::uint32_t left_ms) noexcept;

} // namespace breeze::agent
