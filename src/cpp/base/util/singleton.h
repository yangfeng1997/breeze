// 单例基类（CRTP）：Singleton<T> 进程内一份，ThreadLocalSingleton<T> 每线程一份。
//
//   class Registry : public breeze::Singleton<Registry>
//   {
//       friend class breeze::Singleton<Registry>; // 构造函数私有，要让基类进得来
//       Registry() = default;
//   public:
//       void Add(...);
//   };
//   Registry::Instance().Add(...);
//
// 用函数内 static / thread_local：C++11 起首次进入的初始化由标准保证线程安全，不需要自己写双检锁，也没有 new / delete。
//
// 析构顺序：static 局部对象按构造的逆序在 exit() 时析构。单例之间互相依赖时，被依赖的要先构造（先调一次 Instance()）。
// 进程退出阶段别在别的静态对象的析构里再去碰单例：它可能已经被析构了。
// ThreadLocalSingleton 在线程退出时析构；主线程的那份在 exit() 时析构。

#pragma once

namespace breeze
{

template <typename T>
class Singleton
{
public:
    static T& Instance()
    {
        static T inst;
        return inst;
    }

    Singleton(const Singleton&) = delete;
    Singleton& operator=(const Singleton&) = delete;

protected:
    Singleton() = default;
    ~Singleton() = default;
};

template <typename T>
class ThreadLocalSingleton
{
public:
    static T& Instance()
    {
        thread_local T inst;
        return inst;
    }

    ThreadLocalSingleton(const ThreadLocalSingleton&) = delete;
    ThreadLocalSingleton& operator=(const ThreadLocalSingleton&) = delete;

protected:
    ThreadLocalSingleton() = default;
    ~ThreadLocalSingleton() = default;
};

} // namespace breeze
