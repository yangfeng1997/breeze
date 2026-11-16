#include "base/lua_binding/sandbox.h"

#include <chrono>
#include <cstdlib>

#include "base/lua_binding/check.h"

namespace breeze::lua_binding {
namespace {

char kSandboxKey;

struct SandboxState {
    std::uint64_t instruction_budget = 0;
    std::uint64_t instruction_used = 0;
    std::uint64_t deadline_ms = 0;
    bool poisoned = false;
};

std::uint64_t SteadyMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

int SandboxStateGc(lua_State* L) {
    // SandboxState 是平凡类型、放在 Lua userdata 里；不能 delete 它（会 free 到 Lua 的内存）。
    auto* state = static_cast<SandboxState*>(lua_touserdata(L, 1));
    if (state != nullptr) {
        state->~SandboxState();
    }
    return 0;
}

void BudgetHook(lua_State* L, lua_Debug* /*ar*/) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kSandboxKey);
    auto* state = static_cast<SandboxState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (state == nullptr) {
        return;
    }
    state->instruction_used += 1000;
    const bool instruction_over =
        state->instruction_budget != 0 && state->instruction_used > state->instruction_budget;
    const bool time_over = state->deadline_ms != 0 && SteadyMs() > state->deadline_ms;
    if (state->poisoned || instruction_over || time_over) {
        state->poisoned = true;
        lua_sethook(L, &BudgetHook, LUA_MASKCOUNT, 1);
        luaL_error(L, "breeze::lua_binding: sandbox budget exceeded");
    }
}

}  // namespace

extern "C" void* SandboxAlloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
    auto* stats = static_cast<AllocStats*>(ud);
    if (nsize == 0) {
        if (ptr != nullptr) {
            stats->used -= osize;
            std::free(ptr);
        }
        return nullptr;
    }
    const std::size_t current = ptr != nullptr ? osize : 0;
    const std::size_t next = stats->used - current + nsize;
    if (stats->limit != 0 && next > stats->limit) {
        ++stats->failures;
        return nullptr;
    }
    void* out = std::realloc(ptr, nsize);
    if (out != nullptr) {
        stats->used = next;
        ++stats->allocations;
    }
    return out;
}

LuaStateOptions MakeSandboxOptions(const SandboxOptions& options, AllocStats& stats) {
    stats.limit = options.memory_limit;
    LuaStateOptions state_options;
    state_options.allocator = &SandboxAlloc;
    state_options.allocator_ud = &stats;
    state_options.libs = options.libs;
    return state_options;
}

void ApplySandbox(LuaState& lua, const SandboxOptions& options) {
    lua_State* L = lua.Raw();

    // 只留文本脚本：dofile/loadfile 直接移除。
    lua_pushnil(L);
    lua_setglobal(L, "dofile");
    lua_pushnil(L);
    lua_setglobal(L, "loadfile");

    if (options.instruction_budget == 0 && options.time_budget_ms == 0) {
        return;
    }

    void* raw = lua_newuserdatauv(L, sizeof(SandboxState), 0);
    auto* state = new (raw) SandboxState;
    state->instruction_budget = options.instruction_budget;
    state->deadline_ms = options.time_budget_ms != 0 ? SteadyMs() + options.time_budget_ms : 0;

    lua_newtable(L);
    lua_pushcfunction(L, &SandboxStateGc);
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kSandboxKey);

    lua_sethook(L, &BudgetHook, LUA_MASKCOUNT, 1000);
}

}  // namespace breeze::lua_binding
