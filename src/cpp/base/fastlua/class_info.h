#pragma once

// 类运行时数据。ObjHeader 永远在 userdata 的最前面；ClassInfo 由 StateData 持有，
// lua_close 时统一释放。PropDesc 是属性描述符，get/set 收到已经 cast 到声明类型的对象指针。
// shared_ptr 实例通过 ObjHeader::holder + destroy_holder 持有控制块，__gc 只释放它。

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "base/fastlua/config.h"

namespace breeze::fastlua {

inline constexpr std::uint64_t kObjCookie = 0x666173746c7561ULL;  // "fastlua"
inline constexpr std::uint32_t kObjOwned = 1U << 0;
inline constexpr std::uint32_t kObjShared = 1U << 1;

enum class Policy : std::uint8_t {
    kOwned,     // Lua 拥有：__gc 时 delete
    kBorrowed,  // C++ 拥有：__gc 不做事
};

struct ObjHeader {
    std::uint64_t cookie = kObjCookie;
    const struct ClassInfo* cls = nullptr;
    void* obj = nullptr;
    std::uint32_t flags = 0;
    std::uint32_t pad = 0;
    // shared_ptr 的持有块；destroy_holder 非空时，__gc 只释放它，不 delete obj。
    void* holder = nullptr;
    void (*destroy_holder)(void*) = nullptr;
};

struct PropDesc {
    void* (*cast)(void*) = nullptr;   // 派生对象 -> 声明属性所在类型；null 表示同类型
    std::function<void(lua_State*, void*)> getter;
    std::function<void(lua_State*, void*)> setter;
};

struct ClassInfo {
    struct Base {
        const ClassInfo* info = nullptr;
        void* (*up)(void*) = nullptr;  // 派生指针 -> 基类指针
    };

    std::string name;
    const void* type_tag = nullptr;
    int mt_ref = LUA_NOREF;
    int methods_ref = LUA_NOREF;
    int getters_ref = LUA_NOREF;
    int setters_ref = LUA_NOREF;
    int class_meta_ref = LUA_NOREF;
    void (*destroy)(ObjHeader*) = nullptr;
    std::vector<Base> ancestors;
    std::vector<PropDesc*> props;  // 本类拥有的描述符，StateData 析构时 delete
    bool allow_fields = false;
    bool dot_call = false;
    Policy policy = Policy::kOwned;
};

template <class T>
inline constexpr char kTypeTag = 0;

}  // namespace breeze::fastlua
