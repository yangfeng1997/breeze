#include "base/fastlua/error.h"

#include <cstdlib>

#include "base/fastlua/check.h"

namespace breeze::fastlua {

[[noreturn]] void RaiseError(lua_State* L, const ErrorSlot& slot) {
    if (slot.arg > 0 && slot.expected != nullptr) {
        luaL_typeerror(L, slot.arg, slot.expected);
    } else if (slot.message[0] != '\0') {
        luaL_error(L, "%s", slot.message);
    } else {
        luaL_error(L, "%s", "fastlua: unknown error");
    }
    // luaL_error 不会返回；这一行只用于让编译器相信 [[noreturn]]。
    std::abort();
}

int PanicHandler(lua_State* L) {
    const char* message = lua_tostring(L, -1);
    LogMessage(message != nullptr ? std::string_view(message)
                                  : std::string_view("fastlua: panic with a non-string error value"));
    std::abort();
}

}  // namespace breeze::fastlua
