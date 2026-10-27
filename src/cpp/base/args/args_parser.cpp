#include "args_parser.h"

#include <stdexcept>

#include "args_impl.h"

namespace breeze
{

ArgsParser::ArgsParser(std::string description)
    : impl_(std::make_unique<Impl>(std::move(description)))
{
}
ArgsParser::~ArgsParser() = default;

void ArgsParser::Add(std::string_view name, std::string& out, std::string_view desc, bool required)
{
    impl_->Add(name, out, desc, required);
}
void ArgsParser::Add(std::string_view name, int& out, std::string_view desc, bool required)
{
    impl_->Add(name, out, desc, required);
}
void ArgsParser::Add(std::string_view name, std::int64_t& out, std::string_view desc, bool required)
{
    impl_->Add(name, out, desc, required);
}
void ArgsParser::Add(std::string_view name, std::uint32_t& out, std::string_view desc, bool required)
{
    impl_->Add(name, out, desc, required);
}
void ArgsParser::Add(std::string_view name, double& out, std::string_view desc, bool required)
{
    impl_->Add(name, out, desc, required);
}

void ArgsParser::AddFlag(std::string_view name, bool& out, std::string_view desc)
{
    try
    {
        impl_->cli.add_flag(std::string(name), out, std::string(desc));
    }
    catch (const CLI::Error& e)
    {
        throw std::logic_error(e.what());
    }
}

ParseResult ArgsParser::Parse(int argc, const char* const* argv)
{
    try
    {
        impl_->cli.parse(argc, argv);
        return {true, 0};
    }
    catch (const CLI::ParseError& e)
    {
        return {false, impl_->cli.exit(e)};
    }
}

} // namespace breeze
