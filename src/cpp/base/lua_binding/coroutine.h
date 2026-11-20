#pragma once

// 协程辅助：
//   Coroutine      把一个 Lua function 包成可 resume 的线程；
//   ResumeToken    可在任意线程析构/取消；析构只减原子引用计数，不碰 VM。
// 取消是协作式的：Coroutine::Resume 在运行期间装一个 count hook，Cancel 只置原子标志，
// hook 看到标志后 luaL_error 中断。协程挂起时 Cancel 只标记，下一次 Resume 直接返回 kCancelled。
// 注意：这个 hook 与 sandbox 的预算 hook 共用 Lua 的单一 hook 槽，二者不要同时在一个协程上开。
// 超时由宿主定时器（如 base/timer 的 TimerWheel）到点调用 Cancel，本库不提供定时器/看门狗。

#include <atomic>
#include <string_view>
#include <utility>

#include "base/lua_binding/check.h"
#include "base/lua_binding/config.h"
#include "base/lua_binding/error.h"
#include "base/lua_binding/function.h"
#include "base/lua_binding/lua_state.h"
#include "base/lua_binding/stack.h"

namespace breeze::lua_binding {

struct CoroutineControl {
    std::atomic<int> refs{1};
    std::atomic<bool> cancelled{false};

    void AddRef() noexcept { refs.fetch_add(1, std::memory_order_relaxed); }
    void Release() noexcept {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }
};

class Coroutine;

class ResumeToken {
public:
    ResumeToken() noexcept = default;
    ResumeToken(const ResumeToken&) = delete;
    ResumeToken& operator=(const ResumeToken&) = delete;
    ResumeToken(ResumeToken&& other) noexcept : control_(other.control_) { other.control_ = nullptr; }
    ResumeToken& operator=(ResumeToken&& other) noexcept {
        if (this != &other) {
            Reset();
            control_ = other.control_;
            other.control_ = nullptr;
        }
        return *this;
    }
    ~ResumeToken() { Reset(); }

    // 只置原子标志，可以在任意线程调用；真正中断发生在属主线程下一次 Resume / hook。
    void Cancel() const noexcept {
        if (control_ != nullptr) {
            control_->cancelled.store(true, std::memory_order_release);
        }
    }
    bool Valid() const noexcept { return control_ != nullptr; }

private:
    friend class Coroutine;
    void Reset() noexcept {
        if (control_ != nullptr) {
            control_->Release();
            control_ = nullptr;
        }
    }

    CoroutineControl* control_ = nullptr;
};

struct ResumeResult {
    bool yielded = false;
    int results = 0;
};

class Coroutine {
public:
    Coroutine() noexcept = default;
    Coroutine(const Coroutine&) = delete;
    Coroutine& operator=(const Coroutine&) = delete;
    Coroutine(Coroutine&& other) noexcept
        : main_(other.main_), co_(other.co_), ref_(other.ref_), done_(other.done_), control_(other.control_) {
        other.main_ = nullptr;
        other.co_ = nullptr;
        other.ref_ = LUA_NOREF;
        other.control_ = nullptr;
    }
    Coroutine& operator=(Coroutine&& other) noexcept {
        if (this != &other) {
            Reset();
            main_ = other.main_;
            co_ = other.co_;
            ref_ = other.ref_;
            done_ = other.done_;
            control_ = other.control_;
            other.main_ = nullptr;
            other.co_ = nullptr;
            other.ref_ = LUA_NOREF;
            other.control_ = nullptr;
        }
        return *this;
    }
    ~Coroutine() { Reset(); }

    static std::expected<Coroutine, Error> Create(LuaStateView lua, const LuaFunction& function) {
        lua_State* main = lua.Raw();
        lua_State* co = lua_newthread(main);
        BREEZE_LUA_CHECK(co != nullptr, "breeze::lua_binding: lua_newthread failed");
        const int thread_index = lua_gettop(main);

        function.Push();
        lua_xmove(main, co, 1);  // move the function onto the new thread

        lua_pushvalue(main, thread_index);
        const int ref = luaL_ref(main, LUA_REGISTRYINDEX);
        lua_pop(main, 1);  // pop the thread; the registry ref keeps it alive

        Coroutine out;
        out.main_ = main;
        out.co_ = co;
        out.ref_ = ref;
        out.control_ = new CoroutineControl;
        return out;
    }

    ResumeToken Token() const {
        ResumeToken token;
        if (control_ != nullptr) {
            control_->AddRef();
            token.control_ = control_;
        }
        return token;
    }

    template <class... Args>
    std::expected<ResumeResult, Error> Resume(Args&&... args) {
        if (main_ == nullptr || co_ == nullptr || done_) {
            return Fail(ErrorCode::kInvalidState, "breeze::lua_binding: coroutine is not resumable");
        }
        if (control_ != nullptr && control_->cancelled.load(std::memory_order_acquire)) {
            done_ = true;
            return Fail(ErrorCode::kCancelled, "breeze::lua_binding: coroutine cancelled");
        }
        if (!lua_checkstack(co_, static_cast<int>(sizeof...(Args)) + 4)) {
            return Fail(ErrorCode::kStack, "breeze::lua_binding: coroutine stack overflow");
        }
        PushArgs(co_, std::forward<Args>(args)...);

        const CoroutineControl* previous = t_current_control_;
        t_current_control_ = control_;
        lua_sethook(co_, &ResumeHook, LUA_MASKCOUNT, 1000);

        int results = 0;
        const int status = lua_resume(co_, main_, static_cast<int>(sizeof...(Args)), &results);

        lua_sethook(co_, nullptr, 0, 0);
        t_current_control_ = previous;

        if (status != LUA_OK && status != LUA_YIELD) {
            std::string message = "breeze::lua_binding: coroutine failed";
            if (const char* text = lua_tostring(co_, -1); text != nullptr) {
                message = text;
            }
            lua_pop(co_, 1);
            done_ = true;
            if (control_ != nullptr && control_->cancelled.load(std::memory_order_acquire)) {
                return Fail(ErrorCode::kCancelled, "breeze::lua_binding: coroutine cancelled");
            }
            return Fail(ErrorCode::kLuaError, std::move(message));
        }

        const bool yielded = status == LUA_YIELD;
        if (!yielded) {
            done_ = true;
        }
        lua_pop(co_, results);  // v1 只报告状态；需要值请用 Raw() 上的视图
        return ResumeResult{yielded, results};
    }

    bool Done() const noexcept { return done_; }
    lua_State* Raw() const noexcept { return co_; }

private:
    static void ResumeHook(lua_State* L, lua_Debug* /*ar*/) {
        const CoroutineControl* control = t_current_control_;
        if (control != nullptr && control->cancelled.load(std::memory_order_acquire)) {
            luaL_error(L, "breeze::lua_binding: coroutine cancelled");
        }
    }

    void Reset() noexcept {
        if (co_ != nullptr && main_ != nullptr) {
            lua_closethread(co_, main_);
        }
        if (ref_ != LUA_NOREF && main_ != nullptr) {
            luaL_unref(main_, LUA_REGISTRYINDEX, ref_);
        }
        if (control_ != nullptr) {
            control_->Release();
            control_ = nullptr;
        }
        co_ = nullptr;
        main_ = nullptr;
        ref_ = LUA_NOREF;
    }

    lua_State* main_ = nullptr;
    lua_State* co_ = nullptr;
    int ref_ = LUA_NOREF;
    bool done_ = false;
    CoroutineControl* control_ = nullptr;

    inline static thread_local const CoroutineControl* t_current_control_ = nullptr;
};

}  // namespace breeze::lua_binding
