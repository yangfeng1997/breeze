// Uri：RFC 3986 的示例与相对引用、"没有 / 空"的区分、authority 的各种形态、端口边界、IPv6 与 IPvFuture 字面量、
// 非法输入、非 ASCII、默认端口、ToString 还原、百分号编解码与查询串，
// 以及两份独立的随机对照：RFC 原文语法翻成的正则（加附录 B 的切分正则）、inet_pton。
// 无测试框架依赖：失败即 abort。

#include "uri.h"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#if __has_include(<arpa/inet.h>)
#include <arpa/inet.h>
#define HAVE_INET_PTON 1
#endif

using namespace breeze;

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

namespace
{

[[noreturn]] void Fail(const char* what, std::string_view text)
{
    std::fprintf(stderr, "%s: [%.*s]\n", what, static_cast<int>(text.size()), text.data());
    std::abort();
}

bool SameOpt(const std::optional<std::string>& got, const char* want)
{
    return want == nullptr ? !got.has_value() : (got.has_value() && *got == want);
}

// nullptr 表示 nullopt；port < 0 表示没有端口。
struct Want
{
    const char* scheme;
    const char* userinfo;
    const char* host;
    int port;
    const char* path;
    const char* query;
    const char* fragment;
};

void Expect(std::string_view text, const Want& w)
{
    const auto u = Uri::Parse(text);
    if (!u)
        Fail("应当解析成功", text);
    const bool ok = u->scheme() == w.scheme && SameOpt(u->userinfo(), w.userinfo) && SameOpt(u->host(), w.host) &&
                    (w.port < 0 ? !u->port().has_value() : (u->port().has_value() && *u->port() == w.port)) &&
                    u->path() == w.path && SameOpt(u->query(), w.query) && SameOpt(u->fragment(), w.fragment);
    if (!ok)
        Fail("组件不符", text);
    const auto again = Uri::Parse(u->ToString());
    if (!again || !(*again == *u))
        Fail("ToString 再解析不等", text);
}

void ExpectInvalid(std::string_view text)
{
    if (Uri::Parse(text).has_value())
        Fail("应当被拒绝", text);
}

// ======================================================================================================
// 1. RFC 3986 里的示例

void TestRfcExamples()
{
    Expect("ftp://ftp.is.co.za/rfc/rfc1808.txt",
           {"ftp", nullptr, "ftp.is.co.za", -1, "/rfc/rfc1808.txt", nullptr, nullptr});
    Expect("http://www.ietf.org/rfc/rfc2396.txt",
           {"http", nullptr, "www.ietf.org", -1, "/rfc/rfc2396.txt", nullptr, nullptr});
    Expect("ldap://[2001:db8::7]/c=GB?objectClass?one",
           {"ldap", nullptr, "[2001:db8::7]", -1, "/c=GB", "objectClass?one", nullptr});
    Expect("mailto:John.Doe@example.com", {"mailto", nullptr, nullptr, -1, "John.Doe@example.com", nullptr, nullptr});
    Expect("news:comp.infosystems.www.servers.unix",
           {"news", nullptr, nullptr, -1, "comp.infosystems.www.servers.unix", nullptr, nullptr});
    Expect("tel:+1-816-555-1212", {"tel", nullptr, nullptr, -1, "+1-816-555-1212", nullptr, nullptr});
    Expect("telnet://192.0.2.16:80/", {"telnet", nullptr, "192.0.2.16", 80, "/", nullptr, nullptr});
    Expect("urn:oasis:names:specification:docbook:dtd:xml:4.1.2",
           {"urn", nullptr, nullptr, -1, "oasis:names:specification:docbook:dtd:xml:4.1.2", nullptr, nullptr});
    Expect("foo://example.com:8042/over/there?name=ferret#nose",
           {"foo", nullptr, "example.com", 8042, "/over/there", "name=ferret", "nose"});
    Expect("foo://user@sylar.com:8042/over/there?name=ferret#nose",
           {"foo", "user", "sylar.com", 8042, "/over/there", "name=ferret", "nose"});
}

// RFC 3986 §5.4.1 / §5.4.2：作为引用它们都是合法的，原样还原。
void TestRfcRelativeReferences()
{
    const char* const refs[] = {
        "g:h",        "g",       "./g",        "g/",      "/g",       "//g",    "?y",
        "g?y",        "#s",      "g#s",        "g?y#s",   ";x",       "g;x",    "g;x?y#s",
        "",           ".",       "./",         "..",      "../",      "../g",   "../..",
        "../../",     "../../g", "../../../g", "/./g",    "/../g",    "g.",     ".g",
        "g..",        "..g",     "./../g",     "./g/.",   "g/./h",    "g/../h", "g;x=1/./y",
        "g;x=1/../y", "g?y/./x", "g?y/../x",   "g#s/./x", "g#s/../x", "http:g", "http://a/b/c/d;p?q",
    };
    for (const char* r : refs)
    {
        const auto u = Uri::Parse(r);
        if (!u)
            Fail("相对引用应当解析成功", r);
        if (u->ToString() != r)
            Fail("ToString 应当原样还原", r);
    }
    Expect("g:h", {"g", nullptr, nullptr, -1, "h", nullptr, nullptr});
    Expect("//g", {"", nullptr, "g", -1, "", nullptr, nullptr});
    Expect("?y", {"", nullptr, nullptr, -1, "", "y", nullptr});
    Expect("#s", {"", nullptr, nullptr, -1, "", nullptr, "s"});
    Expect("g?y#s", {"", nullptr, nullptr, -1, "g", "y", "s"});
    Expect("", {"", nullptr, nullptr, -1, "", nullptr, nullptr});
    // 原版丢掉这些 path
    Expect("/a/b?x=1", {"", nullptr, nullptr, -1, "/a/b", "x=1", nullptr});
    Expect("a/b:c", {"", nullptr, nullptr, -1, "a/b:c", nullptr, nullptr});
    Expect("/a:b", {"", nullptr, nullptr, -1, "/a:b", nullptr, nullptr});
}

// ======================================================================================================
// 2. "没有"和"空"的区分

void TestAbsentVersusEmpty()
{
    Expect("http://a/", {"http", nullptr, "a", -1, "/", nullptr, nullptr});
    Expect("http://a/?", {"http", nullptr, "a", -1, "/", "", nullptr});
    Expect("http://a/#", {"http", nullptr, "a", -1, "/", nullptr, ""});
    Expect("http://a/?#", {"http", nullptr, "a", -1, "/", "", ""});
    Expect("http://a", {"http", nullptr, "a", -1, "", nullptr, nullptr}); // 不补 "/"
    Expect("http://@a/", {"http", "", "a", -1, "/", nullptr, nullptr});   // 空 userinfo 也保留
    Expect("http://:@a/", {"http", ":", "a", -1, "/", nullptr, nullptr});
    Expect("file:///etc/hosts", {"file", nullptr, "", -1, "/etc/hosts", nullptr, nullptr});
    Expect("//", {"", nullptr, "", -1, "", nullptr, nullptr});
    Expect("http:", {"http", nullptr, nullptr, -1, "", nullptr, nullptr});
    Expect("http://h:0/", {"http", nullptr, "h", 0, "/", nullptr, nullptr});
    Expect("http://h:/", {"http", nullptr, "h", -1, "/", nullptr, nullptr}); // 原版在这里抛异常

    CHECK(Uri::Parse("")->ToString().empty());
    CHECK(*Uri::Parse("") == Uri());
}

// ======================================================================================================
// 3. authority

void TestAuthority()
{
    Expect("http://u:p@h:81/p?q#f", {"http", "u:p", "h", 81, "/p", "q", "f"});
    Expect("http://user%40x:pw@h/", {"http", "user%40x:pw", "h", -1, "/", nullptr, nullptr});
    Expect("http://[::1]:8080/p", {"http", nullptr, "[::1]", 8080, "/p", nullptr, nullptr});
    Expect("http://[::1]/p", {"http", nullptr, "[::1]", -1, "/p", nullptr, nullptr});
    Expect("http://[::1]:/p", {"http", nullptr, "[::1]", -1, "/p", nullptr, nullptr});
    Expect("http://u@[v1.x]:1/", {"http", "u", "[v1.x]", 1, "/", nullptr, nullptr});
    Expect("http://H.EXAMPLE.com/", {"http", nullptr, "H.EXAMPLE.com", -1, "/", nullptr, nullptr}); // host 原样
    Expect("http://a-b_c~d.e%20f!$&'()*+,;=/",
           {"http", nullptr, "a-b_c~d.e%20f!$&'()*+,;=", -1, "/", nullptr, nullptr});
    Expect("http://1.2.3.4:9/", {"http", nullptr, "1.2.3.4", 9, "/", nullptr, nullptr});
    // 看起来像 IPv4 但不是：按 reg-name 合法（RFC 语法如此），要不要拒绝是调用方解析 IP 时的事
    Expect("http://999.1.1.1/", {"http", nullptr, "999.1.1.1", -1, "/", nullptr, nullptr});
    // 没有 '/' 的 authority 以 '?' / '#' 结束
    Expect("http://h?q", {"http", nullptr, "h", -1, "", "q", nullptr});
    Expect("http://h#f", {"http", nullptr, "h", -1, "", nullptr, "f"});
    Expect("http://h:8?q", {"http", nullptr, "h", 8, "", "q", nullptr});
}

void TestPorts()
{
    Expect("http://h:0/", {"http", nullptr, "h", 0, "/", nullptr, nullptr});
    Expect("http://h:80/", {"http", nullptr, "h", 80, "/", nullptr, nullptr});
    Expect("http://h:65535/", {"http", nullptr, "h", 65535, "/", nullptr, nullptr});
    Expect("http://h:0080/", {"http", nullptr, "h", 80, "/", nullptr, nullptr}); // 前导零
    Expect("http://h:000000000000000000000000080/", {"http", nullptr, "h", 80, "/", nullptr, nullptr});
    ExpectInvalid("http://h:65536/");
    ExpectInvalid("http://h:99999/");
    ExpectInvalid("http://h:99999999999/"); // 原版悄悄变成 1215752191
    ExpectInvalid("http://h:4294967296/");  // 2^32：回绕成 0 的话会被当成合法
    ExpectInvalid("http://h:4294967376/");  // 2^32 + 80：回绕成 80
    ExpectInvalid("http://h:-1/");
    ExpectInvalid("http://h:+80/");
    ExpectInvalid("http://h:8 0/");
    ExpectInvalid("http://h:0x50/");
    ExpectInvalid("http://h:80a/");
    ExpectInvalid("http://h:80:90/");
    ExpectInvalid("http://h::80/");
    ExpectInvalid("http://[::1]:80x/");
    ExpectInvalid("http://[::1]:80:/");
    ExpectInvalid("http://[::1]80/");
}

// ======================================================================================================
// 4. IPv6 / IPvFuture 字面量

const char* const kValidIPv6[] = {
    "::",
    "::1",
    "1::",
    "1::2",
    "1:2:3:4:5:6:7:8",
    "1:2:3:4:5:6:7::",
    "::2:3:4:5:6:7:8",
    "1:2:3:4:5:6:1.2.3.4",
    "::1.2.3.4",
    "::ffff:192.0.2.128",
    "1::1.2.3.4",
    "1:2:3:4:5::1.2.3.4",
    "2001:db8::7",
    "2001:DB8:0:0:8:800:200C:417A",
    "FEDC:BA98:7654:3210:FEDC:BA98:7654:3210",
    "1080::8:800:200C:417A",
    "0:0:0:0:0:0:0:0",
    "0000:0000:0000:0000:0000:0000:0000:0000",
    "::0.0.0.0",
    "::255.255.255.255",
};

const char* const kInvalidIPv6[] = {
    "",
    ":",
    ":::",
    "1",
    "1:2",
    "1:2:3:4:5:6:7",
    "1:2:3:4:5:6:7:8:9",
    "1:2:3:4:5:6:7:8::",
    "::1:2:3:4:5:6:7:8",
    "1::2::3",
    ":1::2",
    "1::2:",
    ":1:2:3:4:5:6:7",
    "1:2:3:4:5:6:7:",
    "12345::",
    "g::",
    "1:2:3:4:5:6:7:1.2.3.4",
    "::1.2.3",
    "::1.2.3.4.5",
    "::256.1.1.1",
    "::01.1.1.1",
    "::1.2.3.-4",
    "::4294967297.1.1.1", // int 回绕成 1
    "::99999999999999999999.1.1.1",
    "::1.2.3.4:1",
    "1.2.3.4::",
    "1.2.3.4",
    "fe80::1%eth0",
    " ::1",
    "::1 ",
};

void TestIPv6Literals()
{
    for (const char* a : kValidIPv6)
    {
        const std::string text = std::string("http://[") + a + "]:7/";
        const auto u = Uri::Parse(text);
        if (!u)
            Fail("IPv6 应当合法", text);
        CHECK(u->host_is_ip_literal() && u->hostname() == a && *u->port() == 7);
    }
    for (const char* a : kInvalidIPv6)
        ExpectInvalid(std::string("http://[") + a + "]/");

    const char* const valid_future[] = {"v1.fe80::a+en1", "V9.a", "vF.x:y", "v1a.~", "v1.!"};
    for (const char* a : valid_future)
    {
        const std::string text = std::string("http://[") + a + "]/";
        const auto u = Uri::Parse(text);
        if (!u)
            Fail("IPvFuture 应当合法", text);
        CHECK(u->host_is_ip_literal() && u->hostname() == a);
    }
    const char* const invalid_future[] = {
        "v.x", "v1.", "v1", "vg.x", "v1.a b", "v1.a/b", "v1x.a", "w1.a", "v1.a%", "", "v", "v.xyz", "V.a:b", "v.~~~"};
    for (const char* a : invalid_future)
        ExpectInvalid(std::string("http://[") + a + "]/");

    ExpectInvalid("http://[]/");
    ExpectInvalid("http://[::1/");
    ExpectInvalid("http://::1]/");
    ExpectInvalid("http://[::1]x/");
    ExpectInvalid("http://[[::1]]/");
    ExpectInvalid("http://h[::1]/");
    ExpectInvalid("http://[::1]]/");
    ExpectInvalid("http://[::1]/[");
    ExpectInvalid("http://a/?q[]=1");
    ExpectInvalid("http://a/p[1]");
}

// ======================================================================================================
// 5. 非法输入

void TestInvalidInputs()
{
    const char* const bad[] = {
        "http://a b/",
        "http://a\tb/",
        "http://a/\x01",
        "http://a/\x7f",
        "http://a/\"",
        "http://a/<",
        "http://a/>",
        "http://a/\\",
        "http://a/^",
        "http://a/`",
        "http://a/{",
        "http://a/|",
        "http://a/}",
        " http://a/",
        "http://a/ ",
        "http://a/\n",
        "http://a/\r",
        // 百分号编码
        "%zz",
        "%1",
        "%",
        "http://a/%",
        "http://a/%4",
        "http://a/%4g",
        "http://a/?%",
        "http://a/#%4",
        "http://a%zz/",
        "http://a%/",
        "http://u%@h/",
        // scheme
        "ht!tp://x",
        "1a:b",
        ":x",
        "+a:b",
        "a b:c",
        "-a:b",
        ".a:b",
        "a_b:c",
        "a:b:c d",
        // 重复的分隔符
        "http://a/#a#b",
        "http://u@v@h/",
        "http://@@h/",
        "http://u:p@@h/",
        "http://a@b@c:80/",
        // 方括号
        "http://h]/",
        "http://[h/",
        "http://a[1]/",
        // host / userinfo 里的非 ASCII 与特殊字符
        "http://a\x80/",
        "http://u\x80@h/",
        "\x80:x",
        "http://a|b/",
        "http://a\\b/",
        "http://a\"b/",
    };
    for (const char* b : bad)
        ExpectInvalid(b);

    // 内嵌的 NUL 字节要按长度处理，不能当成字符串结尾
    ExpectInvalid(std::string_view("http://a/\0b", 11));
    ExpectInvalid(std::string_view("http://a\0b/", 11));
    ExpectInvalid(std::string_view("http://a/?\0", 11));
    ExpectInvalid(std::string_view("\0", 1));
    ExpectInvalid(std::string_view("http://a/#\0", 11));
}

void TestNonAscii()
{
    // 直接贴进来的 UTF-8：path / query / fragment 放行，原样保留
    Expect("http://h/\xe4\xb8\xad\xe6\x96\x87?\xe5\x8f\x82=\xe5\x80\xbc#\xe9\x94\x9a",
           {"http", nullptr, "h", -1, "/\xe4\xb8\xad\xe6\x96\x87", "\xe5\x8f\x82=\xe5\x80\xbc", "\xe9\x94\x9a"});
    Expect("/\xff\xfe", {"", nullptr, nullptr, -1, "/\xff\xfe", nullptr, nullptr}); // 不校验 UTF-8
    // scheme / userinfo / host 里不放行
    ExpectInvalid("h\xc3\xa9://x/");
    ExpectInvalid("http://u\xc3\xa9@h/");
    ExpectInvalid("http://h\xc3\xa9/");
    ExpectInvalid("http://[::1\xc3\xa9]/");
}

// ======================================================================================================
// 6. scheme、默认端口、ToString、hostname

void TestSchemeAndDefaultPort()
{
    const auto u = Uri::Parse("HTTP://A.com/");
    CHECK(u && u->scheme() == "http" && *u->host() == "A.com"); // 只有 scheme 转小写
    CHECK(u->EffectivePort() == 80);                            // 原版这里是 0
    CHECK(Uri::Parse("WSS://h")->EffectivePort() == 443);
    CHECK(Uri::Parse("ws://h:81")->EffectivePort() == 81);
    CHECK(Uri::Parse("https://h")->EffectivePort() == 443);
    CHECK(Uri::Parse("ftp://h")->EffectivePort() == 0);
    CHECK(Uri::Parse("/relative")->EffectivePort() == 0);
    CHECK(Uri::Parse("http://h:0/")->EffectivePort() == 0); // 显式的 :0 就是 0，不回落到默认端口
    CHECK(Uri::Parse("http://h:443/")->EffectivePort() == 443);
    CHECK(Uri::Parse("a+b-c.d://x")->scheme() == "a+b-c.d");

    CHECK(Uri::DefaultPort("http") == 80 && Uri::DefaultPort("ws") == 80);
    CHECK(Uri::DefaultPort("https") == 443 && Uri::DefaultPort("wss") == 443);
    CHECK(Uri::DefaultPort("HTTPS") == 443 && Uri::DefaultPort("Ws") == 80);
    CHECK(Uri::DefaultPort("") == 0 && Uri::DefaultPort("ftp") == 0 && Uri::DefaultPort("https ") == 0);
    CHECK(Uri::DefaultPort("httpsxxxxxxxxxxxx") == 0 && Uri::DefaultPort("htt") == 0 && Uri::DefaultPort("wsss") == 0);
}

void TestToString()
{
    const char* const canonical[] = {
        "http://a.com",
        "http://a.com/",
        "http://a.com:80/",
        "http://a.com/x?y=1#z",
        "mailto:a@b.c",
        "/a/b?x=1",
        "?q",
        "#f",
        "http://a/?",
        "http://a/#",
        "file:///etc/hosts",
        "//h/p",
        "http://[::1]:8/p",
        "urn:isbn:0451450523",
        "http://@a/",
        "a:b",
        "http://a/%7Efoo/%e4",
        "x://h:0/",
    };
    for (const char* c : canonical)
    {
        const auto u = Uri::Parse(c);
        if (!u || u->ToString() != c)
            Fail("ToString 应当原样还原", c);
    }
    // 不原样的只有端口写法和 scheme 大小写
    CHECK(Uri::Parse("http://h:/")->ToString() == "http://h/");
    CHECK(Uri::Parse("http://h:0080/")->ToString() == "http://h:80/");
    CHECK(Uri::Parse("HTTP://h/")->ToString() == "http://h/");
    CHECK(Uri::Parse("http://[::1]:/")->ToString() == "http://[::1]/");
}

void TestHostname()
{
    CHECK(Uri::Parse("http://h/")->hostname() == "h" && !Uri::Parse("http://h/")->host_is_ip_literal());
    CHECK(Uri::Parse("http://[::1]/")->hostname() == "::1" && Uri::Parse("http://[::1]/")->host_is_ip_literal());
    CHECK(Uri::Parse("/p")->hostname().empty() && !Uri::Parse("/p")->host_is_ip_literal());
    CHECK(Uri::Parse("file:///x")->hostname().empty() && !Uri::Parse("file:///x")->host_is_ip_literal());
    CHECK(Uri::Parse("")->hostname().empty());
}

void TestValueSemantics()
{
    const auto a = Uri::Parse("http://u@h:8/p?q#f");
    Uri b = *a;
    CHECK(b == *a);
    Uri c = std::move(b);
    CHECK(c == *a);
    CHECK(!(*a == *Uri::Parse("http://u@h:8/p?q#g")));
    CHECK(!(*Uri::Parse("http://a/?") == *Uri::Parse("http://a/"))); // 空 query 与没有 query 不同
    CHECK(!(*Uri::Parse("http://@a/") == *Uri::Parse("http://a/")));
    CHECK(!(*Uri::Parse("http://h:0/") == *Uri::Parse("http://h/")));
    CHECK(*Uri::Parse("http://h:/") == *Uri::Parse("http://h/")); // 空端口就是没写
}

// ======================================================================================================
// 7. 百分号编码与查询串

void TestPercent()
{
    CHECK(PercentEncode("").empty());
    CHECK(PercentEncode("abcXYZ019-._~") == "abcXYZ019-._~");
    CHECK(PercentEncode(" ") == "%20" && PercentEncode(" ", true) == "+");
    CHECK(PercentEncode("a b+c", true) == "a+b%2Bc" && PercentEncode("a b+c") == "a%20b%2Bc");
    CHECK(PercentEncode("/?#[]@!$&'()*+,;=:") == "%2F%3F%23%5B%5D%40%21%24%26%27%28%29%2A%2B%2C%3B%3D%3A");
    CHECK(PercentEncode("\xe4\xb8\xad") == "%E4%B8%AD");
    CHECK(PercentEncode(std::string_view("\0\xff", 2)) == "%00%FF");
    CHECK(PercentEncode("%") == "%25");

    CHECK(PercentDecode("")->empty());
    CHECK(*PercentDecode("abc") == "abc");
    CHECK(*PercentDecode("%e4%B8%aD") == "\xe4\xb8\xad"); // 大小写十六进制都认
    CHECK(*PercentDecode("a%2Bb") == "a+b" && *PercentDecode("a%20b") == "a b");
    CHECK(*PercentDecode("a+b") == "a+b" && *PercentDecode("a+b", true) == "a b");
    CHECK(*PercentDecode("%2B", true) == "+"); // 编码过的 '+' 解出来还是 '+'，不会变成空格
    CHECK(*PercentDecode("%00%ff") == std::string("\0\xff", 2));
    CHECK(*PercentDecode("%2525") == "%25");                 // 只解一层
    CHECK(*PercentDecode("\xe4\xb8\xad") == "\xe4\xb8\xad"); // 非 ASCII 原样
    CHECK(!PercentDecode("%") && !PercentDecode("%4") && !PercentDecode("%zz") && !PercentDecode("%4g"));
    CHECK(!PercentDecode("a%") && !PercentDecode("%a") && !PercentDecode("%%41") && !PercentDecode("%-1"));
    CHECK(PercentDecode("%41")->at(0) == 'A' && *PercentDecode("%7e") == "~");

    // 往返：任意字节串，两种空格模式
    std::mt19937_64 rng(7);
    for (int i = 0; i < 20000; ++i)
    {
        std::string s(rng() % 20, '\0');
        for (char& c : s)
            c = static_cast<char>(rng());
        CHECK(PercentDecode(PercentEncode(s)) == s);
        CHECK(PercentDecode(PercentEncode(s, true), true) == s);
        const std::string enc = PercentEncode(s, true);
        for (const char c : enc)
            CHECK(c == '%' || c == '+' || std::string_view("-._~").find(c) != std::string_view::npos ||
                  (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'F'));
    }
}

using Pairs = std::vector<std::pair<std::string, std::string>>;

void TestParseQuery()
{
    CHECK(*ParseQuery("") == Pairs{});
    CHECK(*ParseQuery("a=1&b=2") == (Pairs{{"a", "1"}, {"b", "2"}}));
    CHECK(*ParseQuery("a=1&b=%20&c&d=") == (Pairs{{"a", "1"}, {"b", " "}, {"c", ""}, {"d", ""}}));
    CHECK(*ParseQuery("&&a=1&&") == (Pairs{{"a", "1"}})); // 空段跳过
    CHECK(*ParseQuery("a=1=2") == (Pairs{{"a", "1=2"}})); // 第一个 '=' 分隔
    CHECK(*ParseQuery("=v") == (Pairs{{"", "v"}}));
    CHECK(*ParseQuery("=") == (Pairs{{"", ""}}));
    CHECK(*ParseQuery("a=x+y%2Bz") == (Pairs{{"a", "x y+z"}})); // '+' 是空格，%2B 才是加号
    CHECK(*ParseQuery("a%20b=c%26d") == (Pairs{{"a b", "c&d"}}));
    CHECK(*ParseQuery("a+b=c") == (Pairs{{"a b", "c"}}));                             // 键里的 '+' 也是空格
    CHECK(*ParseQuery("k=1&k=2&k=1") == (Pairs{{"k", "1"}, {"k", "2"}, {"k", "1"}})); // 重复的键都保留，保持顺序
    CHECK(*ParseQuery("a=1;b=2") == (Pairs{{"a", "1;b=2"}}));                         // 不认 ';'
    CHECK(*ParseQuery("\xe4\xb8\xad=\xe6\x96\x87") == (Pairs{{"\xe4\xb8\xad", "\xe6\x96\x87"}}));
    CHECK(!ParseQuery("a=%") && !ParseQuery("%zz=1") && !ParseQuery("a=1&b=%4") && !ParseQuery("a=%4g&b=2"));

    // 与 Uri 配合：query 组件直接交给 ParseQuery
    const auto u = Uri::Parse("wss://h/ws?token=a%20b&lang=zh");
    CHECK(u && u->query().has_value());
    CHECK(*ParseQuery(*u->query()) == (Pairs{{"token", "a b"}, {"lang", "zh"}}));
}

// ======================================================================================================
// 8. 随机对照

#ifdef HAVE_INET_PTON
// 与 inet_pton 对 IPv6 的判定逐个对照：字符表里没有 'v'，所以不会走 IPvFuture。
void TestIPv6AgainstInetPton()
{
    std::mt19937 rng(11);
    const std::string alphabet = "0123456789abcdefABCDEF:::::..g%";
    int valid = 0;
    auto check = [&](const std::string& s)
    {
        in6_addr addr{};
        const bool want = inet_pton(AF_INET6, s.c_str(), &addr) == 1;
        const bool got = Uri::Parse("http://[" + s + "]/").has_value();
        if (want != got)
        {
            std::fprintf(stderr, "IPv6 判定与 inet_pton 不一致: [%s] inet_pton=%d 我们=%d\n", s.c_str(), want, got);
            std::abort();
        }
        valid += want;
    };
    for (int i = 0; i < 300000; ++i)
    {
        std::string s(rng() % 26, '\0');
        for (char& c : s)
            c = alphabet[rng() % alphabet.size()];
        check(s);
    }
    // 有结构的生成：随机的组数、"::" 的位置、IPv4 尾巴，再随机破坏一个字符
    for (int i = 0; i < 300000; ++i)
    {
        const int groups = static_cast<int>(rng() % 10);
        std::string s;
        const int dc = (rng() % 3 == 0) ? -1 : static_cast<int>(rng() % (groups + 1));
        for (int g = 0; g < groups; ++g)
        {
            if (g == dc)
                s += "::";
            else if (g > 0)
                s += ':';
            const int len = 1 + static_cast<int>(rng() % 4);
            for (int k = 0; k < len; ++k)
                s += "0123456789abcdef"[rng() % 16];
        }
        if (dc == groups)
            s += "::";
        if (rng() % 4 == 0)
            s += (s.empty() || s.back() == ':' ? "" : ":") + std::to_string(rng() % 300) + ".1.2." +
                 std::to_string(rng() % 300);
        if (rng() % 3 == 0 && !s.empty())
            s[rng() % s.size()] = alphabet[rng() % alphabet.size()];
        check(s);
    }
    CHECK(valid > 20000); // 要真的覆盖到合法的一侧，不能只是一堆非法串
}
#endif

// ---- RFC 3986 原文语法翻成的正则：独立于被测实现的判定依据 ----

struct Oracle
{
    std::regex whole; // URI-reference
    std::regex split; // 附录 B
    std::regex authority;

    Oracle()
    {
        const std::string unres = "A-Za-z0-9\\-._~";
        const std::string sub = "!$&'()*+,;=";
        const std::string pct = "%[0-9A-Fa-f]{2}";
        const std::string pchar = "(?:[" + unres + sub + ":@]|" + pct + ")";
        const std::string userinfo = "(?:[" + unres + sub + ":]|" + pct + ")*";
        const std::string regname = "(?:[" + unres + sub + "]|" + pct + ")*";

        const std::string h16 = "[0-9A-Fa-f]{1,4}";
        const std::string octet = "(?:25[0-5]|2[0-4][0-9]|1[0-9]{2}|[1-9][0-9]|[0-9])";
        const std::string ipv4 = octet + "(?:\\." + octet + "){3}";
        const std::string ls32 = "(?:" + h16 + ":" + h16 + "|" + ipv4 + ")";
        const std::string hc = "(?:" + h16 + ":)";
        auto left = [&](int max_extra)
        { return "(?:(?:" + h16 + ":){0," + std::to_string(max_extra) + "}" + h16 + ")?"; };
        const std::string ipv6 = "(?:" + hc + "{6}" + ls32 + "|::" + hc + "{5}" + ls32 + "|" + "(?:" + h16 +
                                 ")?::" + hc + "{4}" + ls32 + "|" + left(1) + "::" + hc + "{3}" + ls32 + "|" + left(2) +
                                 "::" + hc + "{2}" + ls32 + "|" + left(3) + "::" + hc + ls32 + "|" + left(4) +
                                 "::" + ls32 + "|" + left(5) + "::" + h16 + "|" + left(6) + "::)";
        const std::string ipvfuture = "[vV][0-9A-Fa-f]+\\.[" + unres + sub + ":]+";
        const std::string host = "(?:\\[(?:" + ipv6 + "|" + ipvfuture + ")\\]|" + regname + ")";
        const std::string auth = "(?:" + userinfo + "@)?" + host + "(?::[0-9]*)?";

        const std::string segment = pchar + "*";
        const std::string segnz = pchar + "+";
        const std::string segnznc = "(?:[" + unres + sub + "@]|" + pct + ")+";
        const std::string abempty = "(?:/" + segment + ")*";
        const std::string absolute = "/(?:" + segnz + "(?:/" + segment + ")*)?";
        const std::string rootless = segnz + "(?:/" + segment + ")*";
        const std::string noscheme = segnznc + "(?:/" + segment + ")*";
        const std::string scheme = "[A-Za-z][A-Za-z0-9+.\\-]*";
        const std::string qf = "(?:[" + unres + sub + ":@/?]|" + pct + ")*";

        const std::string hier = "(?://" + auth + abempty + "|" + absolute + "|" + rootless + "|)";
        const std::string rel = "(?://" + auth + abempty + "|" + absolute + "|" + noscheme + "|)";
        const std::string tail = "(?:\\?" + qf + ")?(?:#" + qf + ")?";
        whole = std::regex("^(?:" + scheme + ":" + hier + tail + "|" + rel + tail + ")$");
        split = std::regex("^(([^:/?#]+):)?(//([^/?#]*))?([^?#]*)(\\?([^#]*))?(#(.*))?");
        authority = std::regex("^(?:([^@]*)@)?(\\[[^\\]]*\\]|[^:]*)(?::([0-9]*))?$");
    }
};

struct Parts
{
    std::string scheme;
    std::optional<std::string> userinfo;
    std::optional<std::string> host;
    std::optional<int> port;
    std::string path;
    std::optional<std::string> query;
    std::optional<std::string> fragment;
};

std::string LowerAscii(std::string s)
{
    for (char& c : s)
    {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// 参考判定与切分；只用于 ASCII 输入。
std::optional<Parts> Reference(const Oracle& o, const std::string& s)
{
    if (!std::regex_match(s, o.whole))
        return std::nullopt;
    std::smatch m;
    if (!std::regex_search(s, m, o.split))
        std::abort();
    Parts p;
    if (m[1].matched)
        p.scheme = LowerAscii(m[2].str());
    if (m[3].matched)
    {
        const std::string auth = m[4].str();
        std::smatch a;
        if (!std::regex_match(auth, a, o.authority))
            std::abort();
        if (a[1].matched)
            p.userinfo = a[1].str();
        p.host = a[2].str();
        if (a[3].matched && !a[3].str().empty())
        {
            std::string digits = a[3].str();
            const std::size_t nz = digits.find_first_not_of('0');
            digits = nz == std::string::npos ? "0" : digits.substr(nz);
            if (digits.size() > 5 || std::stoi(digits) > 65535)
                return std::nullopt; // 端口范围不在语法里，是我们另加的限制
            p.port = std::stoi(digits);
        }
    }
    p.path = m[5].str();
    if (m[6].matched)
        p.query = m[7].str();
    if (m[8].matched)
        p.fragment = m[9].str();
    return p;
}

bool SameParts(const Uri& u, const Parts& p)
{
    return u.scheme() == p.scheme && u.userinfo() == p.userinfo && u.host() == p.host &&
           (p.port.has_value() ? (u.port().has_value() && *u.port() == *p.port) : !u.port().has_value()) &&
           u.path() == p.path && u.query() == p.query && u.fragment() == p.fragment;
}

// 往返不变量：解析成功的任何输入，ToString 再解析得到同一个值，且 ToString 是不动点。
void CheckRoundTrip(const Uri& u, std::string_view text)
{
    const std::string s = u.ToString();
    const auto again = Uri::Parse(s);
    if (!again || !(*again == u) || again->ToString() != s)
        Fail("往返不变量被破坏", text);
}

const char* const kSchemes[] = {"http", "HTTPS", "ws", "a", "a+b-c.d", "1a", "a b", "", "x_y", "Foo", "a:b"};
const char* const kUserinfos[] = {"u", "u:p", "u%40", "%zz", "a@b", "", "::", "~!$&'()*+,;=", "u[", "%4"};
const char* const kHosts[] = {"h",
                              "H.Example.COM",
                              "a-b_c",
                              "",
                              "[::1]",
                              "[1:2:3:4:5:6:7:8]",
                              "[v1.x]",
                              "[::",
                              "::1]",
                              "[1:2:3:4:5:6:7]",
                              "[::ffff:1.2.3.4]",
                              "1.2.3.4",
                              "256.1.1.1",
                              "a b",
                              "a%41",
                              "a%4",
                              "a[1]",
                              "[::1]x",
                              "[]",
                              "[1::2::3]",
                              "h:1",
                              "%",
                              "a$b",
                              "a|b"};
const char* const kPorts[] = {"", "80", "0", "65535", "65536", "999999999999", "8a", "-1", " 1", "0080", "4294967376"};
const char* const kPaths[] = {"",    "/",  "/a/b",   "a/b",  "/a:b", "a:b",   "//x",  "/%41", "/%zz", "/a b",
                              "/\\", "/[", "/a;b=c", "/a@b", ":x",   "/./..", "a//b", "/%",   "~",    "/a%2Fb"};
const char* const kQueries[] = {"", "a=1&b=2", "a[]=1", "%", "/?", "a b", "x:y@z", "#", "%41%zz", "a=b/c?d"};

template <std::size_t N>
const char* Pick(std::mt19937& rng, const char* const (&arr)[N])
{
    return arr[rng() % N];
}

std::string GenerateAscii(std::mt19937& rng)
{
    std::string s;
    const int mode = static_cast<int>(rng() % 10);
    if (mode < 6)
    {
        // 按结构拼：每个组件从"合法 + 非法"的候选里挑，还可能整个省略
        if (rng() % 4 != 0)
        {
            s += Pick(rng, kSchemes);
            s += ':';
        }
        if (rng() % 4 != 0)
        {
            s += "//";
            if (rng() % 3 == 0)
            {
                s += Pick(rng, kUserinfos);
                s += '@';
            }
            s += Pick(rng, kHosts);
            if (rng() % 3 == 0)
            {
                s += ':';
                s += Pick(rng, kPorts);
            }
        }
        s += Pick(rng, kPaths);
        if (rng() % 3 == 0)
        {
            s += '?';
            s += Pick(rng, kQueries);
        }
        if (rng() % 3 == 0)
        {
            s += '#';
            s += Pick(rng, kQueries);
        }
    }
    else
    {
        // 特殊字符汤
        const std::string pool = "aZ09-._~!$&'()*+,;=:@/?#[]%\\ <>{}|^`\"\t";
        const std::size_t n = rng() % 14;
        for (std::size_t i = 0; i < n; ++i)
        {
            if (rng() % 8 == 0)
                s += "%4" + std::string(1, "0123456789abcdefg"[rng() % 17]);
            else if (rng() % 10 == 0)
                s += "//";
            else
                s += pool[rng() % pool.size()];
        }
    }
    // 变异：插入 / 删除 / 替换一个字符
    if (rng() % 4 == 0 && !s.empty())
    {
        const std::string pool = ":/?#@[]%.a0 ";
        const std::size_t pos = rng() % s.size();
        switch (rng() % 3)
        {
            case 0:
                s.insert(s.begin() + static_cast<std::ptrdiff_t>(pos), pool[rng() % pool.size()]);
                break;
            case 1:
                s.erase(pos, 1);
                break;
            default:
                s[pos] = pool[rng() % pool.size()];
                break;
        }
    }
    return s;
}

void TestAgainstGrammarOracle()
{
    const Oracle oracle;
    std::mt19937 rng(2026);
    int accepted = 0;
    int rejected = 0;
    constexpr int kRounds = 60000;
    for (int i = 0; i < kRounds; ++i)
    {
        const std::string s = GenerateAscii(rng);
        const auto want = Reference(oracle, s);
        const auto got = Uri::Parse(s);
        if (want.has_value() != got.has_value())
        {
            std::fprintf(stderr, "判定不一致: [%s] 参考=%d 我们=%d\n", s.c_str(), want.has_value(), got.has_value());
            std::abort();
        }
        if (!got)
        {
            ++rejected;
            continue;
        }
        ++accepted;
        if (!SameParts(*got, *want))
            Fail("切分结果与参考不一致", s);
        CheckRoundTrip(*got, s);
    }
    std::printf("  语法对照：%d 个接受、%d 个拒绝\n", accepted, rejected);
    CHECK(accepted > kRounds / 10 && rejected > kRounds / 10); // 两侧都要有足够多的样本，对照才有意义
}

// 任意字节（含 NUL、>= 0x80、控制字符）：不崩、不越界；接受的话往返不变量成立。asan 下跑能抓越界读。
void TestArbitraryBytes()
{
    std::mt19937 rng(99);
    const std::string pool = ":/?#@[]%.a0 \x01\x7f\x80\xff";
    int accepted = 0;
    for (int i = 0; i < 300000; ++i)
    {
        std::string s(rng() % 24, '\0');
        const bool structured = rng() % 2 == 0;
        for (char& c : s)
            c = structured ? pool[rng() % pool.size()] : static_cast<char>(rng());
        const auto u = Uri::Parse(s);
        if (u)
        {
            ++accepted;
            CheckRoundTrip(*u, s);
        }
    }
    CHECK(accepted > 1000);
}

void TestLongInputs()
{
    constexpr std::size_t kBig = 4 << 20;
    CHECK(Uri::Parse("/" + std::string(kBig, 'a')).has_value());
    CHECK(Uri::Parse("http://h/?" + std::string(kBig, 'a')).has_value());
    CHECK(Uri::Parse("http://" + std::string(kBig, 'a') + "/").has_value());
    CHECK(Uri::Parse("http://h/" + std::string(kBig, '/')).has_value());
    CHECK(!Uri::Parse("http://" + std::string(kBig, '[')).has_value());
    CHECK(!Uri::Parse("http://[" + std::string(kBig, ':')).has_value());
    CHECK(!Uri::Parse("http://[" + std::string(kBig, ':') + "]/").has_value());
    CHECK(!Uri::Parse("http://h:" + std::string(kBig, '9')).has_value());
    CHECK(Uri::Parse("http://h:" + std::string(kBig, '0') + "80/")->port() == 80);
    CHECK(!Uri::Parse(std::string(kBig, '%')).has_value());
    CHECK(!Uri::Parse(std::string(kBig, ':')).has_value());
    CHECK(!Uri::Parse("http://" + std::string(kBig, '@')).has_value());
    CHECK(Uri::Parse(std::string(kBig, 'a') + ":x").has_value()); // 超长 scheme 也是线性的
    CHECK(PercentDecode(std::string(kBig, 'a'))->size() == kBig);
    CHECK(PercentEncode(std::string(kBig, ' ')).size() == kBig * 3);
}

} // namespace

int main()
{
    TestRfcExamples();
    TestRfcRelativeReferences();
    TestAbsentVersusEmpty();
    TestAuthority();
    TestPorts();
    TestIPv6Literals();
    TestInvalidInputs();
    TestNonAscii();
    TestSchemeAndDefaultPort();
    TestToString();
    TestHostname();
    TestValueSemantics();
    TestPercent();
    TestParseQuery();
#ifdef HAVE_INET_PTON
    TestIPv6AgainstInetPton();
#endif
    TestAgainstGrammarOracle();
    TestArbitraryBytes();
    TestLongInputs();
    std::puts("uri_test: ok");
    return 0;
}
