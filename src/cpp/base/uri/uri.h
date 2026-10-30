// Uri：RFC 3986 的 URI 引用（绝对 URI 与相对引用）解析与还原，外加百分号编解码与查询串解析。
//
//   auto u = breeze::Uri::Parse("wss://user@Game.example.com:8443/ws/v1?token=a%20b#top");
//   if (!u) ...                 // 语法不合法：nullopt，不抛异常
//   u->scheme();                // "wss"
//   u->userinfo();              // optional "user"
//   u->host();                  // optional "Game.example.com"；IPv6 字面量带方括号："[::1]"
//   u->port();                  // optional 8443（写了才有）
//   u->EffectivePort();         // 8443；没写时取 scheme 的默认端口（http / ws 80，https / wss 443），未知为 0
//   u->path();                  // "/ws/v1"
//   u->query();                 // optional "token=a%20b"
//   u->fragment();              // optional "top"
//   u->ToString();              // 还原成文本
//
// 语义：
//   - 接受 RFC 3986 的 URI-reference：绝对 URI 和相对引用（"/a/b?x=1"、"//host/p"、"?q"、"#f"、""）。
//     要求绝对 URI 的调用方检查 scheme() 非空。
//   - 区分"没有"和"空"：http://a/? 的 query 是空串，http://a/ 的 query 是 nullopt，fragment 同理；
//     host 为 nullopt 表示没有 authority（mailto:x@y、/p），file:///etc/hosts 的 host 是空串。
//   - 语法严格按 RFC：各组件的字符集、"%" 必须跟两位十六进制数字；空格、控制字符、"<>\^`{|}" 一律拒绝；
//     方括号只能出现在 IP 字面量里（查询串里写 a[]=1 要先编码）。
//     唯一的放宽：path / query / fragment 里 >= 0x80 的字节原样放行（直接贴进来的 UTF-8 中文路径），
//     不校验 UTF-8；scheme / userinfo / host 里不放行。
//   - 不做规范化，只有 scheme 转小写：百分号编码不解码、path 不去 "."/".."、host 不转小写、IPv6 不压缩。
//     operator== 是逐组件比较，要比"是不是同一个资源"先自己规范化。
//   - 端口只接受十进制数字，范围 0..65535，超出算语法错误。http://h:/（空端口，RFC 合法）当作没写；
//     前导零接受（:0080 即 80，ToString 输出 80）；显式的 :0 保留（port() == 0），和"没写"区分。
//   - ToString 忠实还原：不补 "/"、不丢空的 "?" 和 "#"、不加 "scheme://"。对解析成功的任何输入，
//     Parse(ToString(u)) == u；端口写法（空端口、前导零）和 scheme 大小写以外，ToString(Parse(s)) == s。
//   - 值类型、不可变（没有 setter），多个线程读同一个对象是安全的。输入长度由调用方限制；解析是线性的、无递归。
//
// 与原版（sylar 的 uri.rl，Ragel 生成）的区别，都是对照着实际跑出来的：
//   - 原版对 http://a:/（语法合法）抛 std::logic_error，一个远端输入就能让进程 terminate。
//   - 原版只在"带 authority"时才保存 path：mailto:x@y、urn:...、tel:... 和相对引用（/a/b?x=1）的 path 全丢，
//     且 dump 出 "mailto:///"、":///?x=1" 这种不合法的东西。
//   - 原版用 atoi 读端口：99999999999 悄悄变成 1215752191，65536 照收，:0 和"没写"分不清。
//   - 原版 dump 会给空 path 补 "/"、丢掉空的 "?" 和 "#"，scheme 不转小写（HTTP:// 的默认端口判断落空）。
//   - 原版的 createAddress 会做阻塞的 DNS 查询，这里不提供；要连接用 hostname() + EffectivePort() 自己解析。
//   - 原版返回 shared_ptr、有不做校验的 setter，这里是值类型加 optional。

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace breeze
{

class Uri
{
public:
    // 空的 Uri 就是 Parse("") 的结果：一个什么都没有的相对引用。
    Uri() = default;

    // 解析一个 URI-reference。语法不合法返回 nullopt。
    static std::optional<Uri> Parse(std::string_view text);

    // 已转小写；相对引用为空串。
    const std::string& scheme() const noexcept { return scheme_; }
    const std::optional<std::string>& userinfo() const noexcept { return userinfo_; }
    // nullopt = 没有 authority；IPv6 / IPvFuture 带方括号，原样（不转小写）。
    const std::optional<std::string>& host() const noexcept { return host_; }
    // 只有写了端口才有值。
    const std::optional<std::uint16_t>& port() const noexcept { return port_; }
    // 原样，可以为空；不会替你补 "/"。
    const std::string& path() const noexcept { return path_; }
    const std::optional<std::string>& query() const noexcept { return query_; }
    const std::optional<std::string>& fragment() const noexcept { return fragment_; }

    // host 去掉 IP 字面量的方括号，可以直接交给 inet_pton / 解析器；没有 host 时是空串。
    // 返回的视图指向本对象，对象活着才有效。
    std::string_view hostname() const noexcept;
    // host 是 "[...]" 形式（IPv6 或 IPvFuture）。
    bool host_is_ip_literal() const noexcept { return host_.has_value() && !host_->empty() && host_->front() == '['; }

    // 显式写了的端口，否则 scheme 的默认端口，再没有就是 0。
    std::uint16_t EffectivePort() const noexcept { return port_.has_value() ? *port_ : DefaultPort(scheme_); }
    // scheme 的默认端口（大小写不敏感）：http / ws 是 80，https / wss 是 443，其余 0（未知）。
    static std::uint16_t DefaultPort(std::string_view scheme) noexcept;

    // 还原成文本，规则见文件头。
    std::string ToString() const;

    friend bool operator==(const Uri&, const Uri&) = default;

private:
    std::string scheme_;
    std::optional<std::string> userinfo_;
    std::optional<std::string> host_;
    std::optional<std::uint16_t> port_;
    std::string path_;
    std::optional<std::string> query_;
    std::optional<std::string> fragment_;
};

// ---- 百分号编码 ----

// 除 unreserved（字母、数字、-._~）以外的字节一律编码成 %XX（大写十六进制），结果可以放进 URI 的任何一个组件。
// space_as_plus 用于 application/x-www-form-urlencoded：空格编码成 '+' 而不是 %20。
std::string PercentEncode(std::string_view s, bool space_as_plus = false);

// 严格解码：'%' 后面必须跟两位十六进制数字，否则返回 nullopt。结果是任意字节串（可以含 NUL、非 UTF-8）。
// plus_as_space 用于 form 编码：'+' 解成空格。
std::optional<std::string> PercentDecode(std::string_view s, bool plus_as_space = false);

// 解析查询串（form 编码）："a=1&b=%20&c&d=" 得到 {a,1} {b," "} {c,""} {d,""}。
// 按 '&' 切，空段跳过，第一个 '=' 分隔键和值，键和值都解码（'+' 当空格）；任何一段解码失败整体 nullopt。
// 不认 ';' 作分隔符；重复的键按出现顺序都保留。
std::optional<std::vector<std::pair<std::string, std::string>>> ParseQuery(std::string_view query);

} // namespace breeze
