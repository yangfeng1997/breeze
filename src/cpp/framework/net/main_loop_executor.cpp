#include "main_loop_executor.h"

#include <algorithm>

#include "connection.h"
#include "connection_context.h"
#include "logger.h"
#include "logger_macros.h"
#include "peer_info.h"

namespace breeze
{

// 每连接的在途计数与暂停状态。IO 线程（入队时加）与主循环线程（分发时减）共用，所以全是原子量；
// 由 shared_ptr 持有：连接对象（经 StateContext）与队列里每条未分发的 Message 事件各持一份，
// 连接在 OnClosed 之后销毁时，主循环上还没消费的事件仍能安全地减计数。
struct MainLoopExecutor::ConnState
{
    ConnectionHandle handle;
    std::atomic<std::uint32_t> msgs{0};
    std::atomic<std::uint64_t> bytes{0};
    // 物理上是否已 PauseRead。IO 线程与主循环线程都可能把它翻转，翻转用 exchange 仲裁：谁拿到旧值谁负责调 PauseRead /
    // ResumeRead。
    std::atomic<bool> paused{false};
    std::atomic<bool> waiting{false}; // 已在 waiters_ 里：只剩全局预算没降下来
};

struct MainLoopExecutor::StateContext final : ConnectionContext
{
    std::shared_ptr<ConnState> state;
};

struct MainLoopExecutor::Event
{
    enum class Kind : std::uint8_t
    {
        Connected,
        Message,
        Closed,
    };
    Kind kind = Kind::Connected;
    CloseReason reason = CloseReason::LocalClose;
    int err = 0;
    ConnectionHandle handle;
    MainLoopHandler* handler = nullptr;
    std::shared_ptr<ConnState> state; // 仅 Message
    SharedBuffer msg;                 // 仅 Message
    std::unique_ptr<PeerInfo> peer;   // 仅 Connected
};

class MainLoopExecutor::Adapter final : public ConnectionHandler
{
public:
    Adapter(MainLoopExecutor& exec, MainLoopHandler& handler)
        : exec_(exec)
        , handler_(handler)
    {
    }
    void OnConnected(Connection& c) noexcept override { exec_.OnConnected(c, handler_); }
    void OnMessage(Connection& c, std::span<const std::byte> payload) noexcept override
    {
        exec_.OnMessage(c, payload, handler_);
    }
    void OnClosed(Connection& c, CloseReason r, int e) noexcept override { exec_.OnClosed(c, r, e, handler_); }

private:
    MainLoopExecutor& exec_;
    MainLoopHandler& handler_;
};

MainLoopExecutor::MainLoopExecutor(App& app, InboundLimits limits)
    : app_(app)
    , limits_(limits)
    , batch_(std::make_unique<Event[]>(kBatch))
{
}

// 在这里定义：销毁队列、适配器、事件需要它们是完整类型。未分发的事件直接随队列销毁，不回调业务。
MainLoopExecutor::~MainLoopExecutor() = default;

ConnectionHandler& MainLoopExecutor::Wrap(MainLoopHandler& handler)
{
    adapters_.push_back(std::make_unique<Adapter>(*this, handler));
    return *adapters_.back();
}

// ---- 在途上限 ----

// 高水位：超过就暂停；低水位：高水位的一半，降到它以下才恢复，留出滞后区间避免来回 epoll_ctl。
bool MainLoopExecutor::ConnOver(const ConnState& st, bool low) const noexcept
{
    const std::size_t msgs = low ? limits_.max_messages_per_conn / 2 : limits_.max_messages_per_conn;
    const std::size_t bytes = low ? limits_.max_bytes_per_conn / 2 : limits_.max_bytes_per_conn;
    return st.msgs.load() > msgs || st.bytes.load() > bytes;
}

// 暂停之后是否还不该恢复：本连接没降到低水位，或全局预算没降到低水位。
bool MainLoopExecutor::StillOver(const ConnState& st) const noexcept
{
    return ConnOver(st, /*low=*/true) || global_bytes_.load() > GlobalLow();
}

void MainLoopExecutor::AddWaiter(const std::shared_ptr<ConnState>& st)
{
    if (st->waiting.exchange(true))
        return;
    const std::lock_guard lk(waiters_mu_);
    waiters_.push_back(st);
    waiters_count_.fetch_add(1);
}

// ---- IO 线程 ----

MainLoopExecutor::Queue& MainLoopExecutor::QueueFor(ConnectionHandle h)
{
    if (!h.valid())
        LOG_FATAL("MainLoopExecutor 只能用于 EventLoopGroup 的连接（独立 loop 上的连接没有句柄）");
    const std::size_t idx = static_cast<std::size_t>(h.group()) * ConnectionHandle::kMaxLoops + h.loop();
    if (Queue* q = table_[idx].load(std::memory_order_acquire))
        return *q;
    // 第一次：该 IO 线程自己创建它专属的队列。少见，加锁；发布顺序保证消费者看到 active_count_ 时对应的指针已写好。
    const std::lock_guard lk(create_mu_);
    Queue* q = table_[idx].load(std::memory_order_relaxed);
    if (q == nullptr)
    {
        owned_.push_back(std::make_unique<Queue>());
        q = owned_.back().get();
        const std::size_t n = active_count_.load(std::memory_order_relaxed);
        active_[n].store(q, std::memory_order_relaxed);
        active_count_.store(n + 1, std::memory_order_release);
        table_[idx].store(q, std::memory_order_release);
    }
    return *q;
}

void MainLoopExecutor::Push(ConnectionHandle h, Event&& ev)
{
    QueueFor(h).Enqueue(std::move(ev));
    app_.Notify(); // 已有 token 时只是一次原子交换
}

void MainLoopExecutor::OnConnected(Connection& c, MainLoopHandler& h)
{
    auto st = std::make_shared<ConnState>();
    st->handle = c.handle();
    auto ctx = std::make_unique<StateContext>();
    ctx->state = std::move(st);
    c.SetContext(std::move(ctx));

    Event ev;
    ev.kind = Event::Kind::Connected;
    ev.handle = c.handle();
    ev.handler = &h;
    ev.peer = std::make_unique<PeerInfo>(c.peer());
    Push(c.handle(), std::move(ev));
}

void MainLoopExecutor::OnMessage(Connection& c, std::span<const std::byte> payload, MainLoopHandler& h)
{
    const std::shared_ptr<ConnState>& st = c.GetContext<StateContext>()->state;
    const std::uint64_t len = payload.size();

    // payload 只在回调内有效，跨线程必须拷贝：IO 线程路径零分配，拷贝只发生在这里。
    Event ev;
    ev.kind = Event::Kind::Message;
    ev.handle = c.handle();
    ev.handler = &h;
    ev.state = st;
    ev.msg = SharedBuffer::CopyFrom(payload);
    // 先记账再入队：主线程分发时减，不会出现先减后加。
    st->msgs.fetch_add(1);
    st->bytes.fetch_add(len);
    global_bytes_.fetch_add(len);
    Push(c.handle(), std::move(ev));

    MaybePause(c, st);
}

void MainLoopExecutor::OnClosed(Connection& c, CloseReason reason, int sys_errno, MainLoopHandler& h)
{
    Event ev;
    ev.kind = Event::Kind::Closed;
    ev.handle = c.handle();
    ev.handler = &h;
    ev.reason = reason;
    ev.err = sys_errno;
    Push(c.handle(), std::move(ev));
}

// 超限就暂停本连接。暂停与恢复由 IO 线程（这里）和主循环线程（Release / ResumeGlobalWaiters）各自判断，
// 用 paused 的 exchange 仲裁，所以必须自己重查一次：主线程可能在我们翻转 paused 之前就把积压消费光了，
// 它看到 paused 还是 false，不会替我们恢复。
void MainLoopExecutor::MaybePause(Connection& c, const std::shared_ptr<ConnState>& st)
{
    if (st->paused.load(std::memory_order_relaxed))
        return;
    // 物理上还暂停着、标志却已被主线程清掉：主线程的 ResumeRead 已经投递、还没执行。此刻翻转标志再 PauseRead 是空操作
    // （物理上本来就是暂停的），随后那个过期的 ResumeRead 生效，连接物理上恢复读而标志仍是 true——之后这里直接返回，
    // 这条连接的在途量就不再受限了。所以等恢复生效再判断：这次 read 剩下的帧照常入队（≤64KB），下一次 read
    // 的消息再暂停。
    if (c.IsPaused())
        return;
    const bool global_over = global_bytes_.load() > limits_.global_budget_bytes;
    if (!global_over && !ConnOver(*st, /*low=*/false))
        return;
    if (st->paused.exchange(true))
        return;
    pauses_.fetch_add(1, std::memory_order_relaxed);
    c.PauseRead();
    // 全局预算还没降到低水位就要排队（不只是"超过高水位"时）：本连接可能没有在途消息，没有 Release 会为它触发，
    // 要靠 ResumeGlobalWaiters 在全局降下来时恢复；"高于低水位、低于高水位"同样阻止恢复。
    if (global_bytes_.load() > GlobalLow())
        AddWaiter(st);
    if (!StillOver(*st) && st->paused.exchange(false))
        c.ResumeRead(); // 已经降下来了
}

// ---- 主循环线程 ----

// 主线程：尝试恢复一条暂停着的连接。返回时它要么已恢复，要么仍暂停但保证有人会再来看它：
//   本连接在途还没降到低水位 → 它有在途消息，之后每条消息的分发（Release）都会再来判断；
//   只剩全局预算没降下来     → 排进 waiters_，全局降到低水位时由 ResumeGlobalWaiters 再来。
// 后一种必须（重新）排队：它可能已经没有在途消息，再也不会有 Release 为它触发；ResumeGlobalWaiters 逐个处理期间
// IO 线程还在往全局里加字节，处理到一半全局又被推高是常态，不能把"这次恢复不了"的连接丢在一边。
void MainLoopExecutor::TryResume(const std::shared_ptr<ConnState>& st)
{
    if (!st->paused.load())
        return;
    if (ConnOver(*st, /*low=*/true))
        return;
    if (global_bytes_.load() > GlobalLow())
    {
        AddWaiter(st);
        if (global_bytes_.load() > GlobalLow()) // 排队之后再查一次：排队期间全局可能已经降下来了
            return;
    }
    if (st->paused.exchange(false))
        st->handle.ResumeRead();
}

// 主线程消费了一条 Message：扣账，并按需恢复读。
void MainLoopExecutor::Release(const std::shared_ptr<ConnState>& st, std::size_t len)
{
    st->msgs.fetch_sub(1);
    st->bytes.fetch_sub(len);
    global_bytes_.fetch_sub(len);
    TryResume(st);
}

// 全局入站字节降到低水位：恢复所有因全局预算而等待的连接（恢复不了的 TryResume 会重新排队）。
void MainLoopExecutor::ResumeGlobalWaiters()
{
    if (waiters_count_.load() == 0 || global_bytes_.load() > GlobalLow())
        return;
    std::vector<std::shared_ptr<ConnState>> w;
    {
        const std::lock_guard lk(waiters_mu_);
        w.swap(waiters_);
        waiters_count_.store(0);
    }
    for (const auto& st : w)
    {
        st->waiting.store(false); // 先清标志：TryResume 里可能要重新排队
        TryResume(st);
    }
}

void MainLoopExecutor::Dispatch(Event& ev)
{
    switch (ev.kind)
    {
        case Event::Kind::Connected:
            ev.handler->OnConnected(ev.handle, *ev.peer);
            break;
        case Event::Kind::Message:
        {
            // 在交给业务之前扣账：从这一刻起 payload 归业务所有，不再占网络层的在途额度。
            Release(ev.state, ev.msg.size());
            ev.handler->OnMessage(ev.handle, std::move(ev.msg));
            break;
        }
        case Event::Kind::Closed:
            ev.handler->OnClosed(ev.handle, ev.reason, ev.err);
            break;
    }
    ev = Event{}; // 释放 shared_ptr / SharedBuffer，批缓冲复用
}

bool MainLoopExecutor::Drain(std::size_t limit)
{
    std::size_t ran = 0;
    const std::size_t n = active_count_.load(std::memory_order_acquire);
    // 各队列轮转着取：从上次停下的下一条队列开始，一条队列的积压不会饿死别的。
    for (std::size_t k = 0; k < n && ran < limit; ++k)
    {
        Queue* q = active_[(rr_ + k) % n].load(std::memory_order_relaxed);
        while (ran < limit)
        {
            const std::size_t want = std::min(kBatch, limit - ran);
            const std::size_t got = q->TryDequeueBulk(batch_.get(), want);
            for (std::size_t i = 0; i < got; ++i)
                Dispatch(batch_[i]);
            ran += got;
            if (got < want)
                break; // 这条队列已空
        }
    }
    if (n > 0)
        rr_ = (rr_ + 1) % n;
    dispatched_.fetch_add(ran, std::memory_order_relaxed);
    ResumeGlobalWaiters();
    return ran >= limit;
}

ExecutorStats MainLoopExecutor::GetStats() const
{
    ExecutorStats s;
    const std::size_t n = active_count_.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < n; ++i)
    {
        const Queue* q = active_[i].load(std::memory_order_relaxed);
        s.queued_events += q->SizeApprox();
        s.queue_peak = std::max<std::uint64_t>(s.queue_peak, q->PeakApprox());
    }
    s.inflight_bytes = global_bytes_.load(std::memory_order_relaxed);
    s.pauses = pauses_.load(std::memory_order_relaxed);
    s.dispatched = dispatched_.load(std::memory_order_relaxed);
    return s;
}

} // namespace breeze
