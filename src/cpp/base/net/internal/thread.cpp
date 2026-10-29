#include "thread.h"

#include <memory>
#include <string>

#include "logger.h"
#include "logger_macros.h"

namespace breeze
{

namespace
{

struct Payload
{
    std::move_only_function<void()> fn;
    std::string name;
};

void* Run(void* arg)
{
    const std::unique_ptr<Payload> p(static_cast<Payload*>(arg));
    if (!p->name.empty())
        pthread_setname_np(pthread_self(), p->name.c_str());
    p->fn();
    return nullptr;
}

} // namespace

Thread::~Thread()
{
    if (joinable_)
        LOG_FATAL("Thread 析构时仍未 Join");
}

int Thread::Start(std::move_only_function<void()> fn, std::string_view name)
{
    if (joinable_)
        LOG_FATAL("Thread 重复 Start");
    auto payload = std::make_unique<Payload>(Payload{std::move(fn), std::string(name.substr(0, 15))});
    const int rc = pthread_create(&tid_, nullptr, &Run, payload.get());
    if (rc != 0)
        return rc; // payload 由 unique_ptr 释放
    payload.release();
    joinable_ = true;
    return 0;
}

void Thread::Join()
{
    if (!joinable_)
        return;
    pthread_join(tid_, nullptr);
    joinable_ = false;
}

} // namespace breeze
