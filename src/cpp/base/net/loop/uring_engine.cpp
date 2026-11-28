#include "base/net/loop/uring_engine.h"

#include <fcntl.h>
#include <liburing.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <vector>

#include "base/container/flat_hash_map.h"
#include "base/logger/logger.h"
#include "base/logger/logger_macros.h"
#include "base/net/loop/io_operation_table.h"
#include "base/net/socket/unique_fd.h"

namespace breeze
{
namespace
{

class UringEngine final : public IoEngine
{
    static constexpr std::uint64_t kPoll = 1;
    static constexpr unsigned kBufferCount = 256;
    static constexpr unsigned kDatagramBytes = 2048;
    static constexpr unsigned kCompletionBudget = 1024;
    static constexpr unsigned kStreamBufferCount = 2048;
    static constexpr unsigned kStreamBufferBytes = 8192;
    static constexpr unsigned kStreamGroupConnections = 2048;
    struct StreamBuffers
    {
        io_uring_buf_ring* ring = nullptr;
        void* bytes = nullptr;
        unsigned attached = 0;
        ~StreamBuffers()
        {
            std::free(ring);
            std::free(bytes);
        }
    };
    static constexpr std::size_t kSendLimit = 256 * 1024;
    enum class Kind
    {
        Read,
        Write,
        DatagramRead,
        DatagramWrite,
        ErrorWatch
    };
    struct State
    {
        IoRegistration id = 0;
        UniqueFd fd;
        IStreamCompletion* stream = nullptr;
        IDatagramCompletion* datagram = nullptr;
        bool alive = true;
        bool reading = true;
        unsigned stream_group = 0;
        ino_t retired_inode = 0;
        bool read_ready = false;
        bool poll_registered = false;
        bool rearm_queued = false;
        unsigned received_this_round = 0;
        std::uint64_t receive_round = 0;
        std::uint64_t read = 0;
        std::uint64_t write = 0;
        std::uint64_t watch = 0;
        std::size_t queued_bytes = 0;
        unsigned queued_datagrams = 0;
        struct DatagramQueue
        {
            std::deque<std::uint64_t> pending;
            unsigned in_flight = 0;
            bool scheduled = false;
            unsigned received_this_round = 0;
            std::uint64_t receive_round = 0;
        };
        std::unique_ptr<DatagramQueue> datagram_queue;
        std::vector<std::byte> deferred;
        bool deferred_event = false;
        bool deferred_eof = false;
        int deferred_error = 0;
    };
    struct Operation
    {
        Kind kind;
        std::shared_ptr<State> state;
        SharedBuffer bytes;
        sockaddr_storage address{};
        iovec iov{};
        std::array<std::byte, 16> prefix{};
        std::array<iovec, 2> owned_iov{};
        unsigned prefix_size = 0;
        std::size_t accounted_bytes = 0;
        msghdr msg{};
        explicit Operation(Kind k, std::shared_ptr<State> s)
            : kind(k)
            , state(std::move(s))
        {
        }
    };
    struct Completion
    {
        std::uint64_t id;
        int result;
        unsigned flags;
    };
    using Operations = detail::IoOperationTable<Operation>;

public:
    explicit UringEngine(UringReceiveMode mode) : native_receive_(mode == UringReceiveMode::Multishot) {}
    ~UringEngine() override
    {
        // queue_exit 先撤销内核引用；buf ring、operation 及数据随后才释放。
        if (initialized_)
        {
            Flush();
            if (sync_cancel_supported_ && (poll_armed_ || !operations_.empty()))
            {
                io_uring_sync_cancel_reg cancel{};
                cancel.flags = IORING_ASYNC_CANCEL_ANY | IORING_ASYNC_CANCEL_ALL;
                cancel.timeout.tv_sec = 2;
                const int rc = io_uring_register_sync_cancel(&ring_, &cancel);
                if (rc < 0 && rc != -ENOENT)
                    LOG_FATAL("io_uring 退出前取消未完成 errno={}", -rc);
            }
            io_uring_queue_exit(&ring_);
        }
        if (buffers_)
            std::free(buffers_);
        if (buffer_ring_)
            std::free(buffer_ring_);
        if (datagram_buffers_)
            std::free(datagram_buffers_);
        if (datagram_ring_)
            std::free(datagram_ring_);
    }

    int Init() override
    {
        epfd_.reset(::epoll_create1(EPOLL_CLOEXEC));
        if (!epfd_.valid())
            return errno;
        io_uring_params params{};
        // 原生multishot可在用户态处理前产生多条完成，CQ独立放大以容纳突发。
        params.flags = IORING_SETUP_CQSIZE;
        params.cq_entries = native_receive_ ? 8192 : 2048;
        const int rc = io_uring_queue_init_params(1024, &ring_, &params);
        if (rc < 0)
            return -rc;
        initialized_ = true;
        if ((params.features & IORING_FEAT_NODROP) == 0)
            return ENOTSUP;
        auto* probe = io_uring_get_probe_ring(&ring_);
        if (!probe)
            return errno != 0 ? errno : ENOTSUP;
        bool supported = true;
        for (int op :
             {IORING_OP_POLL_ADD, IORING_OP_ASYNC_CANCEL, IORING_OP_RECV, IORING_OP_RECVMSG, IORING_OP_SENDMSG})
            supported = supported && io_uring_opcode_supported(probe, op);
        use_send_ = io_uring_opcode_supported(probe, IORING_OP_SEND);
        io_uring_free_probe(probe);
        if (!supported)
            return ENOTSUP;
        io_uring_sync_cancel_reg cancel{};
        cancel.flags = IORING_ASYNC_CANCEL_ANY | IORING_ASYNC_CANCEL_ALL;
        cancel.timeout.tv_sec = 1;
        const int cancel_result = io_uring_register_sync_cancel(&ring_, &cancel);
        if (cancel_result != 0 && cancel_result != -ENOENT)
            return cancel_result == -EINVAL ? ENOTSUP : -cancel_result;
        sync_cancel_supported_ = true;
        if (const int e = ProbeMultishot(); e != 0)
            return e;
        completions_.reserve(kCompletionBudget);
        completion_work_.reserve(kCompletionBudget);
        operations_.Reserve(2048);
        return ArmPoll();
    }

    IoBackend backend() const noexcept override { return IoBackend::IoUring; }
    IoStats stats() const noexcept override { return counters_.Get(); }
    int Add(IPollable* p, int fd, std::uint32_t events) override
    {
        epoll_event ev{};
        ev.events = events;
        ev.data.ptr = p;
        return ::epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, fd, &ev) == 0 ? 0 : errno;
    }
    void Mod(IPollable* p, int fd, std::uint32_t events) override
    {
        epoll_event ev{};
        ev.events = events;
        ev.data.ptr = p;
        if (::epoll_ctl(epfd_.get(), EPOLL_CTL_MOD, fd, &ev) != 0)
            LOG_FATAL("epoll MOD 失败 fd={} errno={}", fd, errno);
    }
    void Del(int fd) override
    {
        if (::epoll_ctl(epfd_.get(), EPOLL_CTL_DEL, fd, nullptr) != 0)
            LOG_FATAL("epoll DEL 失败 fd={} errno={}", fd, errno);
    }

    int Wait(std::span<IoEvent> events, int timeout_ms) override
    {
        StageCancellations();
        if (!native_receive_)
            StageStreamReads();
        StageDatagrams();
        if (!poll_armed_)
            if (const int e = ArmPoll(); e != 0)
                return -e;
        io_uring_cqe* cqe = nullptr;
        int rc;
        if (timeout_ms == 0 || readiness_pending_ || !completions_.empty() || HasDeferred())
        {
            rc = io_uring_submit(&ring_);
            if (rc > 0)
                IoCounters::Add(counters_.submissions, rc);
            if (rc >= 0)
                rc = io_uring_peek_cqe(&ring_, &cqe);
        }
        else
        {
            __kernel_timespec ts{};
            if (timeout_ms > 0)
            {
                ts.tv_sec = timeout_ms / 1000;
                ts.tv_nsec = (timeout_ms % 1000) * 1000000LL;
            }
            rc = io_uring_submit_and_wait_timeout(&ring_, &cqe, 1, timeout_ms < 0 ? nullptr : &ts, nullptr);
            if (rc > 0)
                IoCounters::Add(counters_.submissions, rc);
        }
        if (rc < 0 && rc != -ETIME && rc != -EINTR && rc != -EAGAIN)
            return rc;
        // 批量取得完成项并只发布一次CQ head，避免每个CQE重复读tail/写head。
        std::array<io_uring_cqe*, kCompletionBudget> ready;
        const unsigned room = kCompletionBudget - static_cast<unsigned>(completions_.size());
        const unsigned count = room == 0 ? 0 : io_uring_peek_batch_cqe(&ring_, ready.data(), room);
        int poll_error = 0;
        for (unsigned i = 0; i < count; ++i)
        {
            const auto* item = ready[i];
            const auto id = io_uring_cqe_get_data64(item);
            if (id == kPoll)
            {
                poll_armed_ = false;
                readiness_pending_ = true;
                if (item->res < 0)
                    poll_error = item->res;
            }
            else if (id != 0)
                completions_.push_back({id, item->res, item->flags});
        }
        io_uring_cq_advance(&ring_, count);
        if (poll_error != 0)
            return poll_error;
        if (completions_.size() >= kCompletionBudget)
            IoCounters::Add(counters_.budget_exhaustions);
        if (!readiness_pending_ || events.empty())
            return 0;
        std::array<epoll_event, 1024> native;
        const int n =
            ::epoll_wait(epfd_.get(), native.data(), static_cast<int>(std::min(native.size(), events.size())), 0);
        if (n < 0)
            return errno == EINTR ? 0 : -errno;
        int delivered = 0;
        for (int i = 0; i < n; ++i)
        {
            const auto tag = native[i].data.u64;
            if (tag & 1)
            {
                // IPollable带vptr，指针对齐至少为指针宽度；低位1编码流注册ID，避免悬挂State指针。
                auto state = Find(tag >> 1);
                if (state && state->alive && state->reading && !state->read && !state->read_ready &&
                    !state->deferred_event)
                {
                    state->read_ready = true;
                    stream_ready_.push_back(std::move(state));
                }
            }
            else
                events[delivered++] = {static_cast<IPollable*>(native[i].data.ptr), native[i].events};
        }
        // 本轮满批时readiness集可能仍有余项，不能等待另一条边沿通知。
        readiness_pending_ = n == static_cast<int>(std::min(native.size(), events.size()));
        return delivered;
    }

    void Dispatch() override
    {
        // 回调可 Detach/Attach，先快照，不能拿注册表迭代器跨回调。
        auto deferred = std::move(deferred_);
        for (const auto& state : deferred)
        {
            if (!state->alive || !state->deferred_event || !state->reading)
                continue;
            auto data = std::move(state->deferred);
            const int error = state->deferred_error;
            const bool eof = state->deferred_eof;
            state->deferred_event = false;
            state->deferred_error = 0;
            state->deferred_eof = false;
            std::size_t delivered = 0;
            while (delivered < data.size() && state->alive && state->reading)
            {
                const std::size_t take = std::min<std::size_t>(kStreamBufferBytes, data.size() - delivered);
                state->stream->OnIoRead(std::span<std::byte>(data).subspan(delivered, take), 0);
                delivered += take;
            }
            if (state->alive && delivered < data.size())
            {
                state->deferred.insert(state->deferred.begin(), data.begin() + delivered, data.end());
                state->deferred_event = true;
                state->deferred_eof = eof;
                state->deferred_error = error;
            }
            if (state->alive && delivered == data.size() && (eof || error != 0))
            {
                if (state->reading || error != 0)
                    state->stream->OnIoRead({}, error);
                else
                {
                    state->deferred_event = true;
                    state->deferred_eof = eof;
                    state->deferred_error = error;
                }
            }
            if (state->alive && state->reading && !state->deferred_event)
                RearmRead(state);
        }
        completion_work_.clear();
        completion_work_.swap(completions_);
        const auto& batch = completion_work_;
        ++dispatch_round_;
        const auto budget_end = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
        for (std::size_t i = 0; i < batch.size(); ++i)
        {
            if (i != 0 && i % 32 == 0 && std::chrono::steady_clock::now() >= budget_end)
            {
                completions_.insert(completions_.end(), batch.begin() + i, batch.end());
                IoCounters::Add(counters_.budget_exhaustions);
                break;
            }
            const auto c = batch[i];
            const auto* operation = operations_.Get(c.id);
            bool over_budget = false;
            if (operation && operation->kind == Kind::DatagramRead)
            {
                auto& queue = *operation->state->datagram_queue;
                if (queue.receive_round != dispatch_round_)
                {
                    queue.receive_round = dispatch_round_;
                    queue.received_this_round = 0;
                }
                over_budget = queue.received_this_round++ >= 64;
            }
            else if (operation && operation->kind == Kind::Read)
            {
                auto& state = *operation->state;
                if (state.receive_round != dispatch_round_)
                {
                    state.receive_round = dispatch_round_;
                    state.received_this_round = 0;
                }
                over_budget = state.received_this_round++ >= 16;
            }
            if (over_budget)
                completions_.push_back(c); // 保留 buffer 所有权，下一轮继续，避免单 socket 独占 loop。
            else
                Complete(c);
        }
        // ENOBUFS/取消终止后的重挂统一在本轮归还完 buffer 后进行。
        auto rearm = std::move(rearm_);
        rearm_.clear();
        for (const auto& state : rearm)
        {
            state->rearm_queued = false;
            if (state->alive && state->reading && state->read == 0 && !state->deferred_event)
                RearmRead(state);
        }
    }

    void Drain() override
    {
        if (drained_)
            return;
        draining_ = true;
        cancellations_.clear();
        for (const auto& entry : states_)
        {
            entry.second->alive = false;
            RemoveStreamPoll(*entry.second);
        }
        // 已从 CQ 搬出的完成项也持有请求/缓冲，必须先处理，否则发送队列会停在已完成项上。
        auto staged = std::move(completions_);
        completions_.clear();
        for (const auto c : staged)
            Complete(c);
        // 只在退出线程时等待；常规 Close 不阻塞 loop。可靠发送完成后才撤销余下接收。
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        for (;;)
        {
            Flush();
            const bool sending = operations_.Any([](const Operation& op) { return op.kind == Kind::DatagramWrite; });
            if (!sending || std::chrono::steady_clock::now() >= until)
                break;
            io_uring_cqe* ready = nullptr;
            __kernel_timespec timeout{0, 1000000};
            if (io_uring_wait_cqe_timeout(&ring_, &ready, &timeout) == 0)
            {
                const Completion c{io_uring_cqe_get_data64(ready), ready->res, ready->flags};
                io_uring_cqe_seen(&ring_, ready);
                if (c.id == kPoll)
                    poll_armed_ = false;
                else if (c.id != 0)
                    Complete(c);
            }
        }
        // 长期multishot数量可能很大，先分批取消并持续收割，避免一次同步取消形成退出突发。
        std::deque<std::uint64_t> pending_cancel;
        if (native_receive_)
            operations_.ForEach([&](std::uint64_t id, const Operation&) { pending_cancel.push_back(id); });
        const auto cancel_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (native_receive_ && (!pending_cancel.empty() || !operations_.empty()) && std::chrono::steady_clock::now() < cancel_until)
        {
            unsigned staged_cancel = 0;
            while (!pending_cancel.empty() && staged_cancel < 128)
            {
                const auto id = pending_cancel.front();
                if (!operations_.Get(id))
                {
                    pending_cancel.pop_front();
                    continue;
                }
                auto* sqe = io_uring_get_sqe(&ring_);
                if (!sqe)
                    break;
                pending_cancel.pop_front();
                io_uring_prep_cancel64(sqe, id, 0);
                io_uring_sqe_set_data64(sqe, 0);
                ++staged_cancel;
            }
            Flush();
            io_uring_cqe* ready = nullptr;
            __kernel_timespec timeout{0, 1000000};
            (void)io_uring_wait_cqe_timeout(&ring_, &ready, &timeout);
            unsigned count = 0;
            while (ready && count++ < kCompletionBudget)
            {
                const Completion c{io_uring_cqe_get_data64(ready), ready->res, ready->flags};
                io_uring_cqe_seen(&ring_, ready);
                if (c.id == kPoll)
                    poll_armed_ = false;
                else if (c.id != 0)
                    Complete(c);
                ready = nullptr;
                (void)io_uring_peek_cqe(&ring_, &ready);
            }
        }
        Flush();
        io_uring_sync_cancel_reg cancel{};
        cancel.flags = IORING_ASYNC_CANCEL_ANY | IORING_ASYNC_CANCEL_ALL;
        cancel.timeout.tv_sec = 2;
        const int rc = io_uring_register_sync_cancel(&ring_, &cancel);
        if (rc < 0 && rc != -ENOENT)
            LOG_FATAL("io_uring 停机取消失败 errno={}", -rc);
        io_uring_cqe* cqe = nullptr;
        while (io_uring_peek_cqe(&ring_, &cqe) == 0)
            io_uring_cqe_seen(&ring_, cqe);
        operations_.Clear();
        states_.clear();
        retired_sockets_.clear();
        stream_ready_.clear();
        stream_reads_in_flight_ = 0;
        completions_.clear();
        deferred_.clear();
        rearm_.clear();
        completion_work_.clear();
        datagram_ready_.clear();
        datagram_work_.clear();
        counters_.pending_send_bytes.store(0, std::memory_order_relaxed);
        poll_armed_ = false;
        drained_ = true;
    }

    void Flush() override
    {
        StageCancellations();
        if (!native_receive_)
            StageStreamReads();
        StageDatagrams();
        SubmitPending();
    }
    void SubmitPending()
    {
        while (io_uring_sq_ready(&ring_) != 0)
        {
            const int rc = io_uring_submit(&ring_);
            if (rc == -EINTR)
                continue;
            if (rc <= 0)
                LOG_FATAL("io_uring 提交未前进 result={}", rc);
            IoCounters::Add(counters_.submissions, rc);
        }
    }

    IoRegistration AttachStream(int fd, IStreamCompletion& sink, int& error) override
    {
        unsigned group = 0;
        error = 0;
        if (native_receive_)
            group = SelectStreamGroup(error);
        else if (!buffer_ring_)
        {
            error = SetupBuffers(1, 16 * 1024, buffer_ring_, buffers_);
            if (error == 0)
                IoCounters::Add(counters_.receive_buffer_bytes, kBufferCount * 16 * 1024);
        }
        if (error != 0)
            return 0;
        CancelRetiredSocket(fd);
        auto state = NewState(fd);
        if (!state->fd.valid())
        {
            error = errno;
            states_.erase(state->id);
            return 0;
        }
        state->stream = &sink;
        state->stream_group = group;
        if (group != 0)
            ++stream_groups_[group - 16]->attached;
        error = QueueRead(state);
        if (error)
        {
            if (group != 0)
                --stream_groups_[group - 16]->attached;
            states_.erase(state->id);
            return 0;
        }
        return state->id;
    }
    IoRegistration AttachDatagram(int fd, IDatagramCompletion& sink, int& error) override
    {
        if (!datagram_ring_)
        {
            error = SetupBuffers(2, kDatagramBytes, datagram_ring_, datagram_buffers_);
            if (error)
                return 0;
            IoCounters::Add(counters_.receive_buffer_bytes, kBufferCount * kDatagramBytes);
        }
        CancelRetiredSocket(fd);
        auto state = NewState(fd);
        if (!state->fd.valid())
        {
            error = errno;
            states_.erase(state->id);
            return 0;
        }
        state->datagram = &sink;
        state->datagram_queue = std::make_unique<State::DatagramQueue>();
        error = QueueRead(state);
        if (error)
        {
            states_.erase(state->id);
            return 0;
        }
        return state->id;
    }
    void ReadStream(IoRegistration id, bool enabled) override
    {
        auto state = Find(id);
        if (!state || state->reading == enabled)
            return;
        state->reading = enabled;
        if (!enabled)
        {
            RemoveStreamPoll(*state);
            if (state->read)
                Cancel(state->read);
            QueueErrorWatch(state);
        }
        else
        {
            if (state->watch)
                Cancel(state->watch);
            if (state->deferred_event)
                deferred_.push_back(state);
            else if (!state->read)
                RearmRead(state);
        }
    }
    int WriteStream(IoRegistration id, std::span<const iovec> buffers) override
    {
        auto state = Find(id);
        if (!state || !state->stream)
            return EBADF;
        if (state->write)
            return EBUSY;
        std::size_t size = 0;
        for (const auto& b : buffers)
        {
            if (b.iov_len > kSendLimit - size)
                return EMSGSIZE;
            size += b.iov_len;
        }
        if (!size)
            return EINVAL;
        SharedBuffer bytes = SharedBuffer::Create(size);
        std::size_t off = 0;
        auto output = bytes.MutableBytes();
        for (const auto& b : buffers)
        {
            std::memcpy(output.data() + off, b.iov_base, b.iov_len);
            off += b.iov_len;
        }
        return WriteOwnedStream(id, std::move(bytes));
    }
    int WriteOwnedStream(IoRegistration id, SharedBuffer bytes) override
    {
        auto state = Find(id);
        if (!state || !state->stream)
            return EBADF;
        if (state->write)
            return EBUSY;
        if (bytes.empty() || bytes.size() > kSendLimit)
            return EMSGSIZE;
        auto op = Acquire(Kind::Write, state);
        op->bytes = std::move(bytes);
        return QueueWrite(std::move(op));
    }
    int WriteOwnedStreamV(IoRegistration id, std::span<const std::byte> prefix, SharedBuffer payload) override
    {
        auto state = Find(id);
        if (!state || !state->stream)
            return EBADF;
        if (state->write)
            return EBUSY;
        if (prefix.size() > 16 || payload.size() > kSendLimit - prefix.size() || (prefix.empty() && payload.empty()))
            return EMSGSIZE;
        auto op = Acquire(Kind::Write, state);
        std::copy(prefix.begin(), prefix.end(), op->prefix.begin());
        op->prefix_size = static_cast<unsigned>(prefix.size());
        op->bytes = std::move(payload);
        return QueueWrite(std::move(op));
    }
    int SendDatagram(IoRegistration id,
                     std::span<const std::byte> prefix,
                     std::span<const std::byte> payload,
                     const sockaddr_storage* to,
                     socklen_t length) override
    {
        auto state = Find(id);
        if (!state || !state->datagram)
            return EBADF;
        const std::size_t size = prefix.size() + payload.size();
        if (size > kDatagramBytes)
            return EMSGSIZE;
        if (size > kSendLimit - state->queued_bytes || state->queued_datagrams >= 256 || pending_datagrams_ >= 16384)
            return EAGAIN;
        auto op = Acquire(Kind::DatagramWrite, state);
        op->bytes = SharedBuffer::Create(size);
        if (!prefix.empty())
            std::memcpy(op->bytes.MutableBytes().data(), prefix.data(), prefix.size());
        if (!payload.empty())
            std::memcpy(op->bytes.MutableBytes().data() + prefix.size(), payload.data(), payload.size());
        if (to)
        {
            if (length > sizeof(op->address))
                return EINVAL;
            op->address = *to;
            op->msg.msg_name = &op->address;
            op->msg.msg_namelen = length;
        }
        return QueueWrite(std::move(op));
    }
    void Detach(IoRegistration id) override
    {
        auto state = Find(id);
        if (!state)
            return;
        state->alive = false;
        RemoveStreamPoll(*state);
        if (state->stream_group != 0)
        {
            --stream_groups_[state->stream_group - 16]->attached;
            if (state->read != 0)
            {
                struct stat identity{};
                if (::fstat(state->fd.get(), &identity) == 0)
                {
                    state->retired_inode = identity.st_ino;
                    retired_sockets_[identity.st_ino] = state;
                }
            }
        }
        state->deferred.clear();
        state->deferred_event = false;
        if (state->read)
            Cancel(state->read);
        if (state->datagram && state->queued_datagrams != 0)
        {
            // Close 控制包已接受：停止业务回调，但保留发送队列至完成，不能把关闭通知一起取消。
            states_.erase(id);
            return;
        }
        if (state->write)
            Cancel(state->write);
        if (state->watch)
            Cancel(state->watch);
        if (state->datagram)
        {
            // UDP Close 的契约包含立即释放监听端口；dup 及内核在途引用也必须一起撤销。
            Flush();
            io_uring_sync_cancel_reg cancel{};
            cancel.fd = state->fd.get();
            cancel.flags = IORING_ASYNC_CANCEL_FD | IORING_ASYNC_CANCEL_ALL;
            cancel.timeout.tv_sec = 1;
            const int rc = io_uring_register_sync_cancel(&ring_, &cancel);
            if (rc < 0 && rc != -ENOENT)
                LOG_FATAL("UDP 关闭取消失败 errno={}", -rc);
            state->fd.reset();
        }
        states_.erase(id);
    }

private:
    void CancelRetiredSocket(int fd)
    {
        if (retired_sockets_.empty())
            return;
        struct stat identity{};
        if (::fstat(fd, &identity) != 0)
            return;
        const auto it = retired_sockets_.find(identity.st_ino);
        if (it == retired_sockets_.end())
            return;
        if (auto old = it->second.lock(); old && old->read != 0)
        {
            // 同一socket立即重新注册时，旧multishot不能在取消生效前吞掉新注册的数据。
            Flush();
            io_uring_sync_cancel_reg cancel{};
            cancel.addr = old->read;
            cancel.timeout.tv_sec = 1;
            const int rc = io_uring_register_sync_cancel(&ring_, &cancel);
            if (rc < 0 && rc != -ENOENT)
                LOG_FATAL("旧socket读取取消失败 errno={}", -rc);
        }
        retired_sockets_.erase(identity.st_ino);
    }
    std::shared_ptr<State> NewState(int fd)
    {
        auto state = std::make_shared<State>();
        state->id = next_++;
        // SQE 可能晚于原 fd 关闭才提交；独立 dup 防止数值复用后误投到另一连接。
        state->fd.reset(::fcntl(fd, F_DUPFD_CLOEXEC, 0));
        states_.emplace(state->id, state);
        return state;
    }
    std::shared_ptr<State> Find(IoRegistration id)
    {
        const auto it = states_.find(id);
        return it == states_.end() ? nullptr : it->second;
    }
    bool HasDeferred() const noexcept
    {
        return !deferred_.empty() || !rearm_.empty() || !cancellations_.empty() ||
               (!native_receive_ && !stream_ready_.empty() && stream_reads_in_flight_ < kBufferCount);
    }
    io_uring_sqe* GetSqe()
    {
        auto* sqe = io_uring_get_sqe(&ring_);
        if (!sqe)
        {
            SubmitPending();
            sqe = io_uring_get_sqe(&ring_);
        }
        return sqe;
    }
    int ArmPoll()
    {
        auto* sqe = GetSqe();
        if (!sqe)
            return EBUSY;
        io_uring_prep_poll_add(sqe, epfd_.get(), POLLIN);
        io_uring_sqe_set_data64(sqe, kPoll);
        poll_armed_ = true;
        return 0;
    }
    void Cancel(std::uint64_t operation)
    {
        if (!draining_)
            cancellations_.push_back(operation);
    }
    void StageCancellations()
    {
        // Detach不能在十万连接的关闭遍历内触发enter；分批取消，轮间收割目标CQE释放dup和buffer。
        for (unsigned n = 0; n < 128 && !cancellations_.empty(); ++n)
        {
            if (io_uring_sq_space_left(&ring_) == 0)
                break;
            const auto id = cancellations_.front();
            cancellations_.pop_front();
            if (!operations_.Get(id))
                continue; // 目标可能已经完成，代次检查禁止取消新请求。
            auto* sqe = io_uring_get_sqe(&ring_);
            io_uring_prep_cancel64(sqe, id, 0);
            io_uring_sqe_set_data64(sqe, 0);
            IoCounters::Add(counters_.cancellations);
        }
    }
    void QueueErrorWatch(const std::shared_ptr<State>& state)
    {
        if (state->watch || !state->alive || state->reading)
            return;
        auto* sqe = GetSqe();
        if (!sqe)
            LOG_FATAL("暂停连接的错误监听无法入队");
        const auto id = operations_.Insert(Acquire(Kind::ErrorWatch, state));
        io_uring_prep_poll_add(sqe, state->fd.get(), POLLERR | POLLHUP);
        io_uring_sqe_set_data64(sqe, id);
        state->watch = id;
    }
    std::unique_ptr<Operation> Acquire(Kind kind, const std::shared_ptr<State>& state)
    {
        if (operation_pool_.empty())
            return std::make_unique<Operation>(kind, state);
        auto op = std::move(operation_pool_.back());
        operation_pool_.pop_back();
        op->kind = kind;
        op->state = state;
        return op;
    }
    void Release(std::uint64_t id)
    {
        auto op = operations_.Remove(id);
        if (op->kind == Kind::Read && op->state->retired_inode != 0)
        {
            const auto inode = op->state->retired_inode;
            const auto it = retired_sockets_.find(inode);
            if (it != retired_sockets_.end() && it->second.lock() == op->state)
                retired_sockets_.erase(inode);
        }
        op->state.reset();
        op->bytes = {};
        op->msg = {};
        op->iov = {};
        op->owned_iov = {};
        op->prefix_size = 0;
        op->accounted_bytes = 0;
        op->address = {};
        if (operation_pool_.size() < 2048)
            operation_pool_.push_back(std::move(op));
    }
    void RearmRead(const std::shared_ptr<State>& state)
    {
        if (const int error = QueueRead(state); error != 0)
        {
            // ResumeRead调用栈内也不能同步回调；重挂失败下一轮上报，不能静默停读。
            state->deferred.clear();
            state->deferred_error = error;
            state->deferred_event = true;
            deferred_.push_back(state);
        }
    }
    void ScheduleRead(const std::shared_ptr<State>& state)
    {
        if (!state->rearm_queued)
        {
            state->rearm_queued = true;
            rearm_.push_back(state);
        }
    }
    int QueueRead(const std::shared_ptr<State>& state)
    {
        if (state->read || state->read_ready || !state->alive || !state->reading)
            return 0;
        if (state->stream && !native_receive_)
        {
            if (state->id > (UINT64_MAX >> 1))
                LOG_FATAL("流注册ID耗尽");
            epoll_event event{};
            event.events = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLONESHOT;
            event.data.u64 = (state->id << 1) | 1;
            const int action = state->poll_registered ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
            if (::epoll_ctl(epfd_.get(), action, state->fd.get(), &event) != 0)
                return errno;
            state->poll_registered = true;
            return 0;
        }
        auto* sqe = GetSqe();
        if (!sqe)
        {
            ScheduleRead(state);
            return 0;
        }
        const bool datagram = state->datagram != nullptr;
        auto op = Acquire(datagram ? Kind::DatagramRead : Kind::Read, state);
        if (datagram)
        {
            op->msg.msg_namelen = sizeof(sockaddr_storage);
            io_uring_prep_recvmsg_multishot(sqe, state->fd.get(), &op->msg, 0);
            sqe->buf_group = 2;
        }
        else
        {
            // len=0兼容早期multishot；buf group限制单次字节，内核自己等待socket就绪。
            io_uring_prep_recv_multishot(sqe, state->fd.get(), nullptr, 0, 0);
            sqe->buf_group = static_cast<unsigned short>(state->stream_group);
        }
        sqe->flags |= IOSQE_BUFFER_SELECT;
        const auto id = operations_.Insert(std::move(op));
        io_uring_sqe_set_data64(sqe, id);
        state->read = id;
        return 0;
    }
    void RemoveStreamPoll(State& state)
    {
        if (!state.poll_registered)
            return;
        if (::epoll_ctl(epfd_.get(), EPOLL_CTL_DEL, state.fd.get(), nullptr) != 0)
            LOG_FATAL("流就绪注销失败 errno={}", errno);
        state.poll_registered = false;
    }
    void StageStreamReads()
    {
        // 只给已就绪流分配读取名额；完成项归还buffer前不放出名额，FIFO保证冷连接也能前进。
        while (!draining_ && !stream_ready_.empty() && stream_reads_in_flight_ < kBufferCount)
        {
            auto state = std::move(stream_ready_.front());
            stream_ready_.pop_front();
            state->read_ready = false;
            if (!state->alive || !state->reading || state->read || state->deferred_event)
                continue;
            auto* sqe = GetSqe();
            if (!sqe)
                LOG_FATAL("已就绪流无法提交读取");
            const auto id = operations_.Insert(Acquire(Kind::Read, state));
            // readiness可能被旧请求或外部读取消费；DONTWAIT禁止在无数据时继续占住缓冲名额。
            io_uring_prep_recv(sqe, state->fd.get(), nullptr, 16 * 1024, MSG_DONTWAIT);
            sqe->flags |= IOSQE_BUFFER_SELECT;
            sqe->buf_group = 1;
            io_uring_sqe_set_data64(sqe, id);
            state->read = id;
            ++stream_reads_in_flight_;
        }
    }
    int QueueWrite(std::unique_ptr<Operation> op)
    {
        auto state = op->state;
        const bool datagram = op->kind == Kind::DatagramWrite;
        auto* sqe = datagram ? nullptr : GetSqe();
        if (!datagram && !sqe)
            return EAGAIN;
        op->iov = {const_cast<std::byte*>(op->bytes.data()), op->bytes.size()};
        if (op->prefix_size != 0)
        {
            op->owned_iov[0] = {op->prefix.data(), op->prefix_size};
            op->owned_iov[1] = op->iov;
            op->msg.msg_iov = op->owned_iov.data();
            op->msg.msg_iovlen = op->bytes.empty() ? 1 : 2;
        }
        else
        {
            op->msg.msg_iov = &op->iov;
            op->msg.msg_iovlen = 1;
        }
        op->accounted_bytes = op->bytes.size() + op->prefix_size;
        IoCounters::Add(counters_.pending_send_bytes, op->accounted_bytes);
        Operation* raw = op.get();
        const auto id = operations_.Insert(std::move(op));
        if (datagram)
        {
            state->queued_bytes += raw->bytes.size();
            ++state->queued_datagrams;
            ++pending_datagrams_;
            state->datagram_queue->pending.push_back(id);
        }
        if (datagram)
            ScheduleDatagrams(state);
        else
        {
            state->write = id;
            // 流发送已是连续owned块，不必再让内核导入单iovec/msghdr；数据报仍保留地址语义。
            if (use_send_ && raw->prefix_size == 0)
                io_uring_prep_send(sqe, state->fd.get(), raw->bytes.data(), raw->bytes.size(), MSG_NOSIGNAL);
            else
                io_uring_prep_sendmsg(sqe, state->fd.get(), &raw->msg, MSG_NOSIGNAL);
            io_uring_sqe_set_data64(sqe, id);
        }
        return 0;
    }
    void ScheduleDatagrams(const std::shared_ptr<State>& state)
    {
        auto& queue = *state->datagram_queue;
        if (queue.scheduled || queue.in_flight || queue.pending.empty())
            return;
        queue.scheduled = true;
        datagram_ready_.push_back(state);
    }
    void StageDatagrams()
    {
        datagram_work_.clear();
        datagram_work_.swap(datagram_ready_);
        for (const auto& state : datagram_work_)
        {
            auto& queue = *state->datagram_queue;
            queue.scheduled = false;
            if (queue.in_flight || queue.pending.empty())
                continue;
            const unsigned count = static_cast<unsigned>(std::min<std::size_t>(64, queue.pending.size()));
            // 整条链必须一起发布；HARDLINK 保留顺序但单包失败不连带取消后续数据报。
            if (io_uring_sq_space_left(&ring_) < count)
                SubmitPending();
            if (io_uring_sq_space_left(&ring_) < count)
                LOG_FATAL("UDP 批次没有 SQ 空间");
            for (unsigned i = 0; i < count; ++i)
            {
                const auto id = queue.pending.front();
                queue.pending.pop_front();
                auto& op = *operations_.Get(id);
                auto* sqe = io_uring_get_sqe(&ring_);
                io_uring_prep_sendmsg(sqe, state->fd.get(), &op.msg, MSG_NOSIGNAL | MSG_DONTWAIT);
                if (i + 1 != count)
                    sqe->flags |= IOSQE_IO_HARDLINK;
                io_uring_sqe_set_data64(sqe, id);
            }
            queue.in_flight = count;
        }
        datagram_work_.clear();
    }
    int ProbeMultishot()
    {
        if (const int error = ProbeMultishotSocket(false); error != 0)
            return error;
        return native_receive_ ? ProbeMultishotSocket(true) : 0;
    }
    int ProbeMultishotSocket(bool stream)
    {
        int pair[2];
        if (::socketpair(AF_UNIX, (stream ? SOCK_STREAM : SOCK_DGRAM) | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) != 0)
            return errno;
        UniqueFd receiver(pair[0]), sender(pair[1]);
        io_uring_buf_ring* br = nullptr;
        void* bytes = nullptr;
        int error = SetupBuffers(3, kDatagramBytes, br, bytes);
        if (error)
        {
            std::free(br);
            std::free(bytes);
            return error == EINVAL ? ENOTSUP : error;
        }
        msghdr message{};
        auto* sqe = io_uring_get_sqe(&ring_);
        if (stream)
            io_uring_prep_recv_multishot(sqe, receiver.get(), nullptr, 0, 0);
        else
            io_uring_prep_recvmsg_multishot(sqe, receiver.get(), &message, 0);
        sqe->flags |= IOSQE_BUFFER_SELECT;
        sqe->buf_group = 3;
        io_uring_sqe_set_data64(sqe, 0);
        const char value = 'x';
        (void)::send(sender.get(), &value, 1, MSG_NOSIGNAL);
        io_uring_cqe* completion = nullptr;
        __kernel_timespec timeout{0, 100000000};
        const int rc = io_uring_submit_and_wait_timeout(&ring_, &completion, 1, &timeout, nullptr);
        if (rc < 0 || !completion || completion->res < 0 || !(completion->flags & IORING_CQE_F_MORE))
            error = ENOTSUP;
        if (completion)
            io_uring_cqe_seen(&ring_, completion);
        io_uring_sync_cancel_reg cancel{};
        cancel.fd = receiver.get();
        cancel.flags = IORING_ASYNC_CANCEL_FD | IORING_ASYNC_CANCEL_ALL;
        cancel.timeout.tv_sec = 1;
        const int canceled = io_uring_register_sync_cancel(&ring_, &cancel);
        if (canceled < 0 && canceled != -ENOENT)
            LOG_FATAL("multishot 探测请求无法撤销 errno={}", -canceled);
        while (io_uring_peek_cqe(&ring_, &completion) == 0)
            io_uring_cqe_seen(&ring_, completion);
        (void)io_uring_unregister_buf_ring(&ring_, 3);
        std::free(br);
        std::free(bytes);
        return error;
    }
    unsigned SelectStreamGroup(int& error)
    {
        error = 0;
        for (unsigned i = 0; i < stream_groups_.size(); ++i)
            if (stream_groups_[i]->attached < kStreamGroupConnections)
                return i + 16;
        if (stream_groups_.size() >= 65520)
        {
            error = ENOSPC;
            return 0;
        }
        auto group = std::make_unique<StreamBuffers>();
        const unsigned id = static_cast<unsigned>(stream_groups_.size()) + 16;
        error = SetupBuffers(id, kStreamBufferBytes, group->ring, group->bytes, kStreamBufferCount);
        if (error != 0)
            return 0;
        IoCounters::Add(counters_.receive_buffer_bytes, kStreamBufferCount * kStreamBufferBytes);
        stream_groups_.push_back(std::move(group));
        return id;
    }
    int SetupBuffers(unsigned group, unsigned size, io_uring_buf_ring*& br, void*& bytes, unsigned count = kBufferCount)
    {
        // 全部创建成功才发布成员，Attach 失败后的重试不能误用未注册的 bgid。
        void* data = nullptr;
        if (::posix_memalign(&data, 4096, count * size) != 0)
            return ENOMEM;
        void* ring_memory = nullptr;
        if (::posix_memalign(&ring_memory, 4096, count * sizeof(io_uring_buf)) != 0)
        {
            std::free(data);
            return ENOMEM;
        }
        auto* local = static_cast<io_uring_buf_ring*>(ring_memory);
        std::memset(local, 0, count * sizeof(io_uring_buf));
        io_uring_buf_reg reg{};
        reg.ring_addr = reinterpret_cast<std::uint64_t>(local);
        reg.ring_entries = count;
        reg.bgid = static_cast<unsigned short>(group);
        if (const int rc = io_uring_register_buf_ring(&ring_, &reg, 0); rc < 0)
        {
            std::free(data);
            std::free(local);
            return -rc;
        }
        io_uring_buf_ring_init(local);
        for (unsigned i = 0; i < count; ++i)
            io_uring_buf_ring_add(
                local, static_cast<std::byte*>(data) + i * size, size, i, io_uring_buf_ring_mask(count), i);
        io_uring_buf_ring_advance(local, count);
        bytes = data;
        br = local;
        return 0;
    }
    void ReturnBuffer(const State& state, unsigned id)
    {
        const bool datagram = state.datagram != nullptr;
        const bool grouped = state.stream_group != 0;
        auto* br = datagram ? datagram_ring_ : grouped ? stream_groups_[state.stream_group - 16]->ring : buffer_ring_;
        const auto size = datagram ? kDatagramBytes : grouped ? kStreamBufferBytes : 16 * 1024;
        auto* memory = static_cast<std::byte*>(datagram ? datagram_buffers_ : grouped ? stream_groups_[state.stream_group - 16]->bytes : buffers_);
        const auto count = grouped ? kStreamBufferCount : kBufferCount;
        io_uring_buf_ring_add(br, memory + id * size, size, id, io_uring_buf_ring_mask(count), 0);
        io_uring_buf_ring_advance(br, 1);
    }
    void Complete(const Completion& c)
    {
        IoCounters::Add(counters_.completions);
        if (c.result == -ENOBUFS)
            IoCounters::Add(counters_.receive_buffer_exhaustions);
        Operation* found = operations_.Get(c.id);
        if (found == nullptr)
            LOG_FATAL("io_uring 收到未知或过期请求 id={}", c.id);
        Operation& op = *found;
        auto state = op.state;
        if (op.kind == Kind::ErrorWatch)
        {
            if (state->watch == c.id)
                state->watch = 0;
            if (state->alive && !state->reading && c.result >= 0)
            {
                int error = 0;
                socklen_t length = sizeof(error);
                if (::getsockopt(state->fd.get(), SOL_SOCKET, SO_ERROR, &error, &length) != 0)
                    error = errno;
                if (error != 0)
                    state->stream->OnIoRead({}, error);
                // 只有 FIN/HUP 而无错误时保留 paused：ResumeRead 才交付 EOF，且不重挂 level poll 空转。
            }
            Release(c.id);
            if (state->alive && !state->reading && c.result == -ECANCELED)
                QueueErrorWatch(state);
            return;
        }
        const bool datagram = op.kind == Kind::DatagramRead;
        const bool read = op.kind == Kind::Read || datagram;
        const bool terminal = (c.flags & IORING_CQE_F_MORE) == 0;
        if (terminal)
        {
            if (read && state->read == c.id)
                state->read = 0;
            if (op.kind == Kind::Write && state->write == c.id)
                state->write = 0;
        }
        const bool has_buffer = (c.flags & IORING_CQE_F_BUFFER) != 0;
        const unsigned bid = c.flags >> IORING_CQE_BUFFER_SHIFT;
        if (has_buffer && bid >= (state->stream_group != 0 ? kStreamBufferCount : kBufferCount))
            LOG_FATAL("io_uring 无效 buffer id={}", bid);
        if (read && state->alive)
        {
            if (c.result >= 0)
            {
                if (datagram && has_buffer)
                {
                    auto* data = static_cast<std::byte*>(datagram_buffers_) + bid * kDatagramBytes;
                    auto* out = io_uring_recvmsg_validate(data, c.result, &op.msg);
                    if (out && out->namelen <= sizeof(sockaddr_storage) && !(out->flags & MSG_TRUNC))
                    {
                        sockaddr_storage source{};
                        std::memcpy(&source, io_uring_recvmsg_name(out), out->namelen);
                        const auto n = io_uring_recvmsg_payload_length(out, c.result, &op.msg);
                        state->datagram->OnIoDatagram(
                            source,
                            out->namelen,
                            {static_cast<const std::byte*>(io_uring_recvmsg_payload(out, &op.msg)), n});
                    }
                }
                else if (!datagram)
                {
                    std::span<std::byte> bytes;
                    if (has_buffer)
                        bytes = state->stream_group != 0
                                    ? std::span<std::byte>(static_cast<std::byte*>(stream_groups_[state->stream_group - 16]->bytes) + bid * kStreamBufferBytes, static_cast<std::size_t>(c.result))
                                    : std::span<std::byte>(static_cast<std::byte*>(buffers_) + bid * 16 * 1024, static_cast<std::size_t>(c.result));
                    DeliverStream(state, bytes, 0);
                }
            }
            else if (c.result != -ENOBUFS && c.result != -ECANCELED && c.result != -EAGAIN)
            {
                if (datagram)
                    state->datagram->OnIoDatagramError(-c.result);
                else
                    DeliverStream(state, {}, -c.result);
            }
        }
        else if (!read)
        {
            IoCounters::Add(counters_.pending_send_bytes, 0 - static_cast<std::uint64_t>(op.accounted_bytes));
            if (op.kind == Kind::DatagramWrite)
            {
                state->queued_bytes -= op.bytes.size();
                --state->queued_datagrams;
                --pending_datagrams_;
                if (state->alive && c.result < 0 && c.result != -ECANCELED)
                    state->datagram->OnIoDatagramError(-c.result);
                else if (state->alive && c.result >= 0)
                    state->datagram->OnIoDatagramSent(c.result);
            }
            else if (state->alive)
                state->stream->OnIoWrite(c.result > 0 ? c.result : 0,
                                         c.result < 0    ? -c.result
                                         : c.result == 0 ? EPIPE
                                                         : 0);
        }
        if (has_buffer)
            ReturnBuffer(*state, bid);
        if (terminal)
        {
            const bool datagram_write = op.kind == Kind::DatagramWrite;
            if (op.kind == Kind::Read && state->stream_group == 0)
            {
                if (stream_reads_in_flight_ == 0)
                    LOG_FATAL("流读取名额计数下溢");
                --stream_reads_in_flight_;
            }
            Release(c.id);
            if (datagram_write)
            {
                if (state->datagram_queue->in_flight == 0)
                    LOG_FATAL("UDP 批次计数下溢");
                --state->datagram_queue->in_flight;
                ScheduleDatagrams(state);
            }
            if (read && state->alive && state->reading && !state->deferred_event)
                ScheduleRead(state);
        }
    }
    void DeliverStream(const std::shared_ptr<State>& state, std::span<std::byte> bytes, int error)
    {
        if (state->reading || error != 0)
            state->stream->OnIoRead(bytes, error);
        else
        {
            // multishot取消生效前可能有多条CQE，暂停暂存只能追加，不能覆盖先前字节。
            state->deferred.insert(state->deferred.end(), bytes.begin(), bytes.end());
            if (error != 0)
                state->deferred_error = error;
            state->deferred_eof = state->deferred_eof || (bytes.empty() && error == 0);
            state->deferred_event = true;
        }
    }

    const bool native_receive_;
    io_uring_buf_ring* buffer_ring_ = nullptr;
    void* buffers_ = nullptr;
    unsigned stream_reads_in_flight_ = 0;
    std::deque<std::shared_ptr<State>> stream_ready_;
    IoCounters counters_;
    UniqueFd epfd_;
    io_uring ring_{};
    bool initialized_ = false;
    bool sync_cancel_supported_ = false;
    bool use_send_ = false;
    bool drained_ = false;
    bool draining_ = false;
    bool poll_armed_ = false;
    bool readiness_pending_ = false;
    std::uint64_t dispatch_round_ = 0;
    std::vector<std::unique_ptr<StreamBuffers>> stream_groups_;
    io_uring_buf_ring* datagram_ring_ = nullptr;
    void* datagram_buffers_ = nullptr;
    std::uint64_t next_ = 2;
    std::deque<std::uint64_t> cancellations_;
    unsigned pending_datagrams_ = 0;
    FlatHashMap<IoRegistration, std::shared_ptr<State>> states_;
    FlatHashMap<ino_t, std::weak_ptr<State>> retired_sockets_;
    Operations operations_;
    std::vector<Completion> completions_, completion_work_;
    std::vector<std::shared_ptr<State>> deferred_;
    std::vector<std::shared_ptr<State>> rearm_;
    std::vector<std::shared_ptr<State>> datagram_ready_, datagram_work_;
    std::vector<std::unique_ptr<Operation>> operation_pool_;
};
} // namespace

std::unique_ptr<IoEngine> MakeUringEngine(UringReceiveMode mode) { return std::make_unique<UringEngine>(mode); }
} // namespace breeze
