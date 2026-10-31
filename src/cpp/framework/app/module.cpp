#include "framework/app/module.h"

namespace breeze
{

// 类内没有别的非内联虚函数可作 key function，析构放这里让 vtable 只在本 TU 生成一份。
Module::~Module() = default;

} // namespace breeze
