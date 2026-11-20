// 共享内存通道的握手线格式与握手之后的控制消息。设计见 docs/共享内存IPC设计.md §6.1 / §6.2 / §6.5 / §6.6。
//
// 握手只有两条定长消息，不用 Protobuf：
//   App   ── Hello ──▶ Agent        Attach：不带 fd；Reattach：随这条消息用 SCM_RIGHTS 附上 [memfd, tx_efd, rx_efd]
//   Agent ── Reply ──▶ App          result == 0 即 Accept；Attach 新建 / 恢复时附上 [memfd, tx_efd, rx_efd]，Reattach 不带 fd
//                                   result != 0 即 Reject，原因是 errno，不带 fd
// 握手之后 UDS 只剩 App → Agent 的定长控制消息（Control：PrepareRestart / Bye）；Agent 不会往 UDS 里写任何东西。
// 两端在同一台机器上，所以结构按本机字节序、本机布局直接收发，不做字节序转换；
// 以后改了任何字段就升 kVersion，对端靠头里的魔数与版本识别，不会悄悄错位。
//
// Hello / Control 是对端发来的，**一律当作不可信**：先整条收进本地缓冲再校验（CheckHello / CheckControl），
// 字符串字段不假定以 '\0' 结尾。

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>

namespace breeze::shm::wire
{

inline constexpr std::uint32_t kMagic = 0x4D485342; // 字节序列 'B' 'S' 'H' 'M'
inline constexpr std::uint16_t kVersion = 2;
inline constexpr std::size_t kBusIdSize = 64; // 含结尾的 '\0'，所以名字最长 63 字节

enum class MsgType : std::uint16_t
{
    kHello = 1,
    kReply = 2,
    kControl = 3,
};

enum class AttachType : std::uint32_t
{
    kAttach = 1,   // App 来要通道：新建，或恢复 Agent 保留着的那一条（App 重启，§6.2）
    kReattach = 2, // Agent 重启之后 App 把手里的 fd 传回去恢复（§6.5）
};

// Hello.flags
inline constexpr std::uint32_t kFlagDiscardRetained = 1u << 0; // 只对 Attach：丢弃保留着的队列，按 Hello 里的容量重建

// App 想用的传输方式（Hello.transport）。
enum class Transport : std::uint32_t
{
    kUdsOnly = 1,      // 数据也走 UDS，不用共享内存
    kShmPreferred = 2, // 要共享内存；Agent 给不了（配额 / 关闭了）时允许退化成仅 UDS
    kShmRequired = 3,  // 要共享内存；给不了就 Reject
};

// Agent 实际选定的传输方式（Reply.transport）。
enum class ChosenTransport : std::uint32_t
{
    kUds = 1, // 握手之后这条 UDS 就是数据通道（不带 fd）
    kShm = 2, // 共享内存是数据通道，UDS 只做探活与控制消息
};

struct Header
{
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t type;
};

struct Hello
{
    Header hdr;
    std::uint32_t attach_type; // AttachType
    std::uint32_t flags;       // kFlag*，其余位必须为 0
    std::int32_t pid;          // 对端自称的 pid，只供日志；身份以 SO_PEERCRED 为准
    std::uint32_t transport;   // Transport
    std::uint64_t tx_capacity; // 0 = 默认（16 MiB）；其余必须是 2 的幂且在 [1 MiB, 服务端的单环上限]；Reattach 必须为 0
    std::uint64_t rx_capacity;
    std::uint32_t tx_max_payload; // 0 = 默认（min(1 MiB, 容量 / 16)）；Reattach 必须为 0
    std::uint32_t rx_max_payload;
    char busid[kBusIdSize]; // 通道名，[A-Za-z0-9._:@-]{1,63}，以 '\0' 结尾
};

struct RingParams
{
    std::uint64_t capacity;
    std::uint32_t max_payload;
    std::uint32_t reserved;
};

struct Reply
{
    Header hdr;
    std::int32_t result;       // 0 = Accept；否则是 errno（EBUSY / ENOSPC / EPROTO / EINVAL / EACCES / ENOTSUP ……）
    std::uint32_t resumed;     // 1 = 恢复了保留下来的队列（Attach 恢复，或 Reattach）
    std::uint64_t app_epoch;   // 通道头里的 app_epoch 此刻的值
    std::uint64_t agent_epoch; // 通道头里的 agent_epoch 此刻的值
    std::uint64_t total_size;  // memfd 的总大小，接手方据此与自己从头里算出的布局互相核对
    RingParams tx;
    RingParams rx;
    std::uint32_t transport; // ChosenTransport；Reject 时为 0
    std::uint32_t reserved;  // 必须为 0
};

enum class ControlOp : std::uint32_t
{
    kPrepareRestart = 1, // arg = 保留毫秒数（App 马上要重启，希望队列留这么久）
    kBye = 2,            // arg 必须为 0：App 正常退出，Agent 排空 Tx 之后立刻销毁，不保留
};

// 握手之后 App → Agent 的控制消息，定长。
struct Control
{
    Header hdr; // type = kControl
    std::uint32_t op;
    std::uint32_t arg;
};

static_assert(sizeof(Header) == 8);
static_assert(sizeof(Hello) == 112);
static_assert(sizeof(Reply) == 80);
static_assert(sizeof(Control) == 16);
static_assert(std::is_trivially_copyable_v<Hello> && std::is_trivially_copyable_v<Reply> &&
              std::is_trivially_copyable_v<Control>);

// 通道名合法：1 到 63 个字符，每个都在 [A-Za-z0-9._:@-] 里。
bool IsValidBusId(std::string_view name) noexcept;

// 把名字写进 Hello 的 busid 字段（其余字节清零）。名字不合法返回 false，Hello 不变。
bool SetBusId(Hello& hello, std::string_view name) noexcept;

// 从 Hello 里取通道名：必须在 kBusIdSize 之内以 '\0' 结尾、名字合法，否则返回空。返回的视图指向 hello 内部。
std::optional<std::string_view> GetBusId(const Hello& hello) noexcept;

// 校验一条收到的 Hello 的固定部分（头、类型、标志、传输方式、通道名，Reattach 的容量字段必须为 0）；
// 容量本身由创建通道时校验。
// 返回 0 或 errno：魔数 / 版本 / 消息类型不符 EPROTO；其余不合法 EINVAL。
int CheckHello(const Hello& hello) noexcept;

// 构造头。
Header MakeHeader(MsgType type) noexcept;

// 校验收到的 Reply 的头：魔数、版本、类型都对才返回 0，否则 EPROTO。
int CheckReplyHeader(const Reply& reply) noexcept;

// 校验一条收到的 Control：头不符 EPROTO；op 不认识、arg 与 op 不匹配 EINVAL。
int CheckControl(const Control& control) noexcept;

// 构造 Control（头已填好）。
Control MakeControl(ControlOp op, std::uint32_t arg) noexcept;

} // namespace breeze::shm::wire
