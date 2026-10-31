// 每连接的用户数据：业务把"这条连接上的状态"挂在 Connection 上，省得按句柄再查一次哈希表。设计见 docs/网络库设计.md
// §4.4。
//
// 只在所属 IO 线程读写（经 Connection::SetContext / GetContext），随连接销毁（OnClosed 之后）一起析构。

#pragma once

namespace breeze
{

class ConnectionContext
{
public:
    virtual ~ConnectionContext() = default;
};

} // namespace breeze
