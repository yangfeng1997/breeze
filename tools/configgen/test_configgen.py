"""configgen 的测试。无第三方测试框架：python -m unittest。需要 protoc（schema 由它解析）。

覆盖：schema 校验的各种错误（都要带 文件:行号）、确定性输出、JSON Schema、中文路径、描述符解码器，
以及生成的 Go 代码能 gofmt / go vet / go test。C++ 端到端在 CMake 的 configgen_cpp_test / common_config_test 里。
"""

import json
import re
import shutil
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

import yaml

import configgen
from configgen import SchemaError

HERE = Path(__file__).resolve().parent
OPTIONS_DIR = HERE.parent.parent / "config" / "schema"  # options.proto 在这里
SAMPLE_DIR = HERE / "testdata" / "schema"
CONFIG_DIR = OPTIONS_DIR.parent  # config/：common.yaml 与 env/*.yaml

HEADER = 'syntax = "proto3";\npackage t;\nimport "options.proto";\n'  # 3 行，body 从第 4 行起


class SchemaFiles:
    """一个临时 schema 目录。"""

    def __init__(self, prefix: str = "configgen_test_"):
        self.dir = Path(tempfile.mkdtemp(prefix=prefix))

    def write(self, name: str, text: str, header: bool = True) -> None:
        (self.dir / f"{name}.proto").write_text((HEADER if header else "") + text, encoding="utf-8")

    def load(self, names=("t",), includes=(OPTIONS_DIR,)):
        return configgen.load_schemas(self.dir, includes, None, list(names))

    def cleanup(self):
        shutil.rmtree(self.dir, ignore_errors=True)


def msg(field: str) -> str:
    """一个只有一个字段 f 的 message M。"""
    return f"message M {{\n  {field}\n}}\n"


class DecoderTest(unittest.TestCase):
    """自带的 protobuf 解码器：不依赖 Python 的 protobuf 包，所以自己要靠得住。"""

    def test_varint(self):
        self.assertEqual(configgen._varint(b"\x96\x01", 0), (150, 2))
        self.assertEqual(configgen._varint(b"\x00", 0), (0, 1))
        self.assertEqual(configgen._varint(b"\xff\xff\xff\xff\x0f", 0), (2**32 - 1, 5))

    def test_fields(self):
        # field 1 varint 150；field 2 字符串 "hi"；field 3 fixed64
        data = b"\x08\x96\x01" + b"\x12\x02hi" + b"\x19" + b"\x00" * 7 + b"\x3f"
        got = list(configgen._fields(data))
        self.assertEqual(got[0], (1, 0, 150))
        self.assertEqual(got[1], (2, 2, b"hi"))
        self.assertEqual(got[2][0], 3)
        self.assertEqual(len(got[2][2]), 8)

    def test_packed_varints(self):
        self.assertEqual(configgen._packed_varints(b"\x04\x00\x02\x01"), (4, 0, 2, 1))

    def test_doc_strips_one_leading_space(self):
        self.assertEqual(configgen._doc(" 第一行\n  缩进的第二行\n"), "第一行\n 缩进的第二行")
        self.assertEqual(configgen._doc(""), "")


class NamingTest(unittest.TestCase):
    def test_conversions(self):
        self.assertEqual(configgen.camel("tcp_port"), "tcpPort")
        self.assertEqual(configgen.camel("max_files_per_hour"), "maxFilesPerHour")
        self.assertEqual(configgen.pascal("drop_oldest"), "DropOldest")
        self.assertEqual(configgen.snake("GateConfig"), "gate_config")
        self.assertEqual(configgen.snake("RedisNodeConfig"), "redis_node_config")


class SchemaErrorTest(unittest.TestCase):
    """每一种写错的 schema 都必须被拦下，并且错误里有 文件:行号 和能看懂的原因。"""

    def setUp(self):
        self.tmp = SchemaFiles()
        self.addCleanup(self.tmp.cleanup)

    def check(self, body: str, *needles: str, name: str = "t", header: bool = True, line: "int | None" = None,
              includes=(OPTIONS_DIR,)):
        self.tmp.write(name, body, header=header)
        with self.assertRaises(SchemaError) as cm:
            self.tmp.load((name,), includes)
        text = str(cm.exception)
        self.assertRegex(text, r"\.proto:\d+:", f"错误里没有 文件:行号: {text}")
        for needle in needles:
            self.assertIn(needle, text)
        if line is not None:
            self.assertRegex(text, rf"{name}\.proto:{line}:", f"行号不对: {text}")

    # -- protoc 自己会拦的（原样转出来，路径换回真实路径）-----------------------------------
    def test_protoc_errors_are_passed_through(self):
        self.check(msg("Nope f = 1;"), "Nope")
        self.check(msg('uint32 f = 1 [(config.requird) = true];'), "requird")
        self.check(msg("uint32 f = 1;\n  uint32 g = 1;"), "1")  # 字段编号重复
        self.check(msg("uint32 f = 1;\n  uint32 f = 2;"), "f")  # 字段名重复

    def test_error_line_numbers_point_at_the_offending_field(self):
        # HEADER 3 行；message M { 在第 4 行；f 在第 5 行
        self.check(msg('uint32 f = 1 [(config.required) = true, (config.default) = "1"];'),
                   "required 与 default 矛盾", line=5)
        self.check("message M {\n  uint32 ok = 1;\n  string bad = 2 [(config.range) = {min: 1, max: 2}];\n}\n",
                   "range 只能用在数值类型", line=6)

    # -- option 语义 ------------------------------------------------------------------------
    def test_flags(self):
        self.check(msg('string f = 1 [(config.secret) = true, (config.reloadable) = true];'),
                   "secret 与 reloadable 不能同时")
        self.check(msg('string f = 1 [(config.secret) = true, (config.default) = "x"];'),
                   "secret 字段不能有 default")

    def test_defaults(self):
        def d(t: str, v: str, *needles):
            self.check(msg(f'{t} f = 1 [(config.default) = "{v}"];'), *needles)

        d("int32", "abc", "不是 int32")
        d("uint32", "-1", "不是 uint32")
        d("uint32", "4294967296", "不是 uint32")
        d("bool", "1", "不是 bool")
        d("double", "abc", "不是数字")
        d("double", "nan", "有限的数字")
        self.check("enum E {\n  E_UNSPECIFIED = 0;\n  E_A = 1;\n}\n" + msg('E f = 1 [(config.default) = "c"];'),
                   "不是枚举 E 的取值")
        self.check(msg('repeated int32 f = 1 [(config.default) = "1"];'), "只有标量")
        self.check("message X {\n  int32 a = 1;\n}\n" + msg('X f = 1 [(config.default) = "1"];'), "只有标量")

    def test_range(self):
        def r(t: str, rng: str, *needles, extra: str = ""):
            self.check(msg(f"{t} f = 1 [(config.range) = {{{rng}}}{extra}];"), *needles)

        r("string", "min: 1, max: 2", "range 只能用在数值类型")
        r("int32", "min: 5, max: 1", "下界 5 大于上界 1")
        r("int32", "min: 0.5, max: 2", "必须是整数")
        r("uint32", "min: 0, max: 4294967296", "超出 uint32")
        r("int64", "min: 0, max: 18014398509481984", "2^53")  # 2^54：double 存不准
        r("int32", "min: 1, max: 5", "不在 range", extra=', (config.default) = "9"')

    def test_min_count(self):
        self.check(msg("int32 f = 1 [(config.min_count) = 1];"), "只能用在 repeated / map")
        self.check(msg("repeated int32 f = 1 [(config.min_count) = 0];"), "min_count 至少是 1")

    def test_env_value_only_on_leaf_fields(self):
        self.check("message N {\n  int32 a = 1;\n}\n" + msg("N f = 1 [(config.env_value) = true];"), "env_value 要标在叶子字段")
        self.check("message N {\n  int32 a = 1;\n}\n" + msg("optional N f = 1 [(config.env_value) = true];"),
                   "env_value 要标在叶子字段")

    # -- 类型与结构 -------------------------------------------------------------------------
    def test_unsupported_types_and_constructs(self):
        self.check(msg("bytes f = 1;"), "不支持的字段类型 bytes")
        self.check(msg("sint32 f = 1;"), "sint32")
        self.check(msg("fixed32 f = 1;"), "fixed32")
        self.check(msg("map<int32, int32> f = 1;"), "map 的键只支持 string")
        self.check("message M {\n  message N {\n    int32 a = 1;\n  }\n  int32 f = 1;\n}\n", "嵌套")
        self.check("message M {\n  enum E {\n    E_UNSPECIFIED = 0;\n  }\n  int32 f = 1;\n}\n", "嵌套")
        self.check("message M {\n  oneof o {\n    int32 a = 1;\n    int32 b = 2;\n  }\n}\n", "oneof")
        self.check("service S {\n}\n" + msg("int32 f = 1;"), "service")
        self.check('import "google/protobuf/descriptor.proto";\nextend google.protobuf.FieldOptions {\n  bool x = 60000;\n}\n'
                   + msg("int32 f = 1;"), "extend")
        self.check("message M {\n}\n", "至少要有一个字段")
        self.check('syntax = "proto2";\npackage t;\nmessage M {\n  optional int32 f = 1;\n}\n', "proto3", header=False)
        self.check('syntax = "proto3";\nmessage M {\n  int32 f = 1;\n}\n', "必须声明 package", header=False)

    def test_names(self):
        self.check(msg("int32 fooBar = 1;"), "字段名 'fooBar' 不合法")
        self.check(msg("int32 class = 1;"), "关键字")
        self.check(msg("int32 a_b = 1;\n  int32 a__b = 2;"), "JSON name")  # protoc 先拦：驼峰转换后同名
        self.check("message m_lower {\n  int32 f = 1;\n}\n", "类型名 'm_lower' 不合法")

    def test_enum_rules(self):
        self.check("enum E {\n  E_A = 0;\n}\n" + msg("int32 f = 1;"), "E_UNSPECIFIED = 0")
        self.check("enum E {\n  E_UNSPECIFIED = 0;\n  X_A = 1;\n}\n" + msg("int32 f = 1;"), "E_<大写名字>")
        self.check("enum E {\n  E_UNSPECIFIED = 0;\n  E_A = 5;\n}\n" + msg("int32 f = 1;"), "从 0 起连续")
        self.check("enum E {\n  E_UNSPECIFIED = 0;\n}\n" + msg("int32 f = 1;"), "至少要有一个值")
        # 去前缀 / 忽略大小写后同名：protoc 先拦
        self.check("enum E {\n  E_UNSPECIFIED = 0;\n  E_A_B = 1;\n  E_A__B = 2;\n}\n" + msg("int32 f = 1;"), "same name")

    def test_cycles_and_imports(self):
        self.check("message A {\n  B b = 1;\n}\nmessage B {\n  A a = 1;\n}\n", "循环引用")
        self.check("message A {\n  A a = 1;\n}\n", "循环引用")
        # 引用 options.proto 里的类型：不是 schema 里声明的
        self.check(msg("config.Range f = 1;"), "不是 schema 里声明的类型")
        # import 的文件不在 schema 目录
        other = SchemaFiles("configgen_inc_")
        self.addCleanup(other.cleanup)
        other.write("elsewhere", "message Z {\n  int32 a = 1;\n}\n", header=False)
        (other.dir / "elsewhere.proto").write_text('syntax = "proto3";\npackage e;\nmessage Z {\n  int32 a = 1;\n}\n',
                                                   encoding="utf-8")
        self.check('import "elsewhere.proto";\n' + msg("e.Z f = 1;"), "只能 import 同目录下的 schema",
                   includes=(OPTIONS_DIR, other.dir))

    def test_extern_type_is_gone(self):
        """不再支持引用手写类型：用到的地方读生成的配置、自己赋值。protoc 会拒绝这个已删除的 option。"""
        self.check('message X {\n  option (config.extern_type) = {cpp: "a::X"};\n}\n' + msg("X f = 1;"), "extern_type")

    def test_options_proto_must_match_option_numbers(self):
        """options.proto 改了编号、或者多出了生成器不认识的 option，都要在启动时报错，不能悄悄读错。"""
        orig = (OPTIONS_DIR / "options.proto").read_text(encoding="utf-8")
        for what, text in (("改了编号", orig.replace("= 52001;", "= 59001;")),
                           ("多出一个 option", orig.replace("  bool required = 52001;",
                                                          "  bool required = 52001;\n  bool extra = 52999;"))):
            drift = SchemaFiles("configgen_drift_")
            self.addCleanup(drift.cleanup)
            (drift.dir / "options.proto").write_text(text, encoding="utf-8")
            with self.subTest(what):
                self.check(msg("int32 f = 1;"), "OPTION_NUMBERS", includes=(drift.dir,))

    def test_missing_protoc(self):
        with self.assertRaises(SchemaError) as cm:
            configgen.find_protoc("/nonexistent/protoc")
        self.assertIn("protoc 不存在", str(cm.exception))

    # -- 输出阶段才会发现的 ----------------------------------------------------------------

    def test_go_type_names_must_be_unique_across_schemas(self):
        self.tmp.write("a", "message Same {\n  int32 f = 1;\n}\n")
        self.tmp.write("b", "message Same {\n  int32 f = 1;\n}\n")
        out = self.tmp.dir / "out"
        argv = ["--schema-dir", str(self.tmp.dir), "-I", str(OPTIONS_DIR)]
        # 两个 schema 的 package 都是 t，proto 本身就不允许同名；换成不同 package 才能测出 Go 的限制
        (self.tmp.dir / "b.proto").write_text(
            'syntax = "proto3";\npackage u;\nmessage Same {\n  int32 f = 1;\n}\n', encoding="utf-8")
        self.assertEqual(configgen.main(argv + ["--cpp-out", str(out / "cpp")]), 0)  # C++ 按 package 分命名空间
        self.assertEqual(configgen.main(argv + ["--go-out", str(out / "go")]), 1)    # Go 共用一个包


class GeneratorTest(unittest.TestCase):
    def setUp(self):
        self.out = Path(tempfile.mkdtemp(prefix="configgen_out_"))
        self.addCleanup(shutil.rmtree, self.out, True)

    def gen(self, *extra) -> int:
        return configgen.main(["--schema-dir", str(SAMPLE_DIR), "-I", str(OPTIONS_DIR), *extra])

    def test_sample_generates_all_targets(self):
        self.assertEqual(self.gen("--cpp-out", str(self.out / "cpp"), "--go-out", str(self.out / "go"),
                                  "--jsonschema-out", str(self.out / "json")), 0)
        for rel in ("cpp/sample_config.h", "cpp/sample_types_config.h", "go/sample_config.go",
                    "go/sample_types_config.go", "json/gate_config.schema.json"):
            self.assertTrue((self.out / rel).is_file(), rel)
        self.assertFalse((self.out / "cpp" / "options_config.h").exists(), "options.proto 不该生成")

    def test_real_schema_validates(self):
        self.assertEqual(configgen.main(["--schema-dir", str(OPTIONS_DIR), "--check"]), 0)

    def test_output_is_deterministic_and_unchanged_files_are_not_rewritten(self):
        args = ["--cpp-out", str(self.out / "cpp"), "--go-out", str(self.out / "go")]
        self.assertEqual(self.gen(*args), 0)
        files = sorted(self.out.rglob("*.*"))
        first = {f: f.read_text(encoding="utf-8") for f in files}
        mtimes = {f: f.stat().st_mtime_ns for f in files}
        time.sleep(0.02)
        self.assertEqual(self.gen(*args), 0)
        for f in files:
            self.assertEqual(f.read_text(encoding="utf-8"), first[f])
            self.assertEqual(f.stat().st_mtime_ns, mtimes[f], f"{f.name} 内容没变却被重写了，会触发无谓的重编")

    def test_prune_removes_only_stale_generated_files(self):
        cpp, go = self.out / "cpp", self.out / "go"
        cpp.mkdir()
        go.mkdir()
        for name in ("gone_config.h", "notes.txt"):
            (cpp / name).write_text("x")
        (go / "gone_config.go").write_text("x")
        self.assertEqual(self.gen("--cpp-out", str(cpp), "--go-out", str(go), "--prune"), 0)
        self.assertFalse((cpp / "gone_config.h").exists())
        self.assertFalse((go / "gone_config.go").exists())
        self.assertTrue((cpp / "notes.txt").exists(), "不是生成器产出的文件名，不能删")
        self.assertTrue((cpp / "sample_config.h").exists())

    def test_prune_rejects_partial_schema_selection(self):
        with self.assertRaises(SystemExit):
            self.gen("--cpp-out", str(self.out), "--prune", "--schema", "sample")

    def test_check_mode_writes_nothing(self):
        self.assertEqual(self.gen("--check"), 0)
        self.assertEqual(list(self.out.iterdir()), [])

    def test_banner_and_no_timestamps(self):
        self.gen("--cpp-out", str(self.out / "cpp"), "--go-out", str(self.out / "go"))
        cpp = (self.out / "cpp" / "sample_config.h").read_text(encoding="utf-8")
        go = (self.out / "go" / "sample_config.go").read_text(encoding="utf-8")
        for text in (cpp, go):
            self.assertTrue(text.startswith("// Code generated by tools/configgen from sample.proto. DO NOT EDIT."))
        self.assertIsNone(re.search(r"20\d\d-\d\d-\d\d|\d\d:\d\d:\d\d", cpp + go))

    def test_cpp_namespace_comes_from_proto_package(self):
        self.gen("--cpp-out", str(self.out))
        cpp = (self.out / "sample_config.h").read_text(encoding="utf-8")
        self.assertIn("namespace config::sample\n", cpp)
        self.assertIn("::config::sample_types::Level", cpp)  # import 的包

    def test_cpp_field_tables(self):
        self.gen("--cpp-out", str(self.out))
        cpp = (self.out / "sample_config.h").read_text(encoding="utf-8")
        self.assertIn('Field("tcp_port", &::config::sample::GateConfig::tcpPort).Required().Range(1U, 65535U)',
                      cpp)
        self.assertIn('Field("heartbeat_sec", &::config::sample::GateConfig::heartbeatSec).Reloadable()', cpp)
        self.assertIn('Field("password", &::config::sample::Creds::password).Secret()', cpp)
        self.assertIn('Field("tuning", &::config::sample::GateConfig::tuning).Reloadable()', cpp)
        self.assertIn('{"drop_oldest", ::config::sample::Mode::DropOldest}', cpp)  # 去前缀、转小写
        self.assertNotIn('{"unspecified"', cpp)  # UNSPECIFIED 不是合法的 yaml 取值
        self.assertIn("    Unspecified,", cpp)    # 但 C++ 枚举里有它（0 值，表示没设置）
        self.assertIn('.Check([](const auto& v) -> std::string { return v.size() < 1 ? "至少需要 1 个元素" : ""; })', cpp)
        self.assertIn('#include "sample_types_config.h"', cpp)  # import 的头文件
        self.assertIn("std::uint64_t big = 18446744073709551615ULL;", cpp)

    def test_doc_comes_from_proto_comments(self):
        self.gen("--cpp-out", str(self.out / "cpp"), "--jsonschema-out", str(self.out / "json"))
        cpp = (self.out / "cpp" / "sample_config.h").read_text(encoding="utf-8")
        self.assertIn("    // TCP 监听端口\n    std::uint32_t tcpPort{};", cpp)
        self.assertIn("// 网关服配置。\nstruct GateConfig", cpp)
        doc = json.loads((self.out / "json" / "gate_config.schema.json").read_text(encoding="utf-8"))
        self.assertEqual(doc["definitions"]["GateConfig"]["properties"]["tcp_port"]["description"], "TCP 监听端口")

    def test_json_schema(self):
        self.gen("--jsonschema-out", str(self.out))
        doc = json.loads((self.out / "gate_config.schema.json").read_text(encoding="utf-8"))
        defs = doc["definitions"]
        self.assertEqual(doc["$ref"], "#/definitions/GateConfig")
        gate = defs["GateConfig"]
        self.assertFalse(gate["additionalProperties"])  # 拼错的键在编辑器里直接标红
        self.assertEqual(gate["required"], ["tcp_port"])
        p = gate["properties"]
        self.assertEqual((p["tcp_port"]["minimum"], p["tcp_port"]["maximum"]), (1, 65535))
        self.assertEqual(p["heartbeat_sec"]["default"], 30)
        # draft-07 里 $ref 旁边的 default 会被忽略，所以带 default 时用 allOf 包一层
        self.assertEqual(p["mode"], {"allOf": [{"$ref": "#/definitions/Mode"}], "default": "fast", "x-env-value": True})
        self.assertNotIn("x-env-value", p["heartbeat_sec"])
        self.assertEqual(p["opt"]["anyOf"][1], {"type": "null"})
        self.assertEqual(p["limits"]["additionalProperties"]["type"], "integer")
        self.assertEqual(p["modes"]["items"], {"$ref": "#/definitions/Mode"})
        self.assertEqual(p["seeds"]["minItems"], 1)
        self.assertTrue(defs["Creds"]["properties"]["password"]["writeOnly"])
        self.assertEqual(defs["Mode"]["enum"], ["fast", "safe", "drop_oldest"])  # 没有 unspecified
        self.assertEqual(defs["Level"]["enum"], ["debug", "info", "warn", "error"])
        self.assertEqual(defs["Endpoint"]["required"], ["host", "port"])

    def test_non_ascii_source_path(self):
        """仓库可以放在带中文的路径下（Windows 的 protoc.exe 按 ANSI 读命令行，直接传会乱码）。"""
        src = SchemaFiles(prefix="configgen_源代码_")
        self.addCleanup(src.cleanup)
        for p in SAMPLE_DIR.glob("*.proto"):
            shutil.copyfile(p, src.dir / p.name)
        opts = SchemaFiles(prefix="configgen_选项_")
        self.addCleanup(opts.cleanup)
        shutil.copyfile(OPTIONS_DIR / "options.proto", opts.dir / "options.proto")
        self.assertEqual(configgen.main(["--schema-dir", str(src.dir), "-I", str(opts.dir), "--check"]), 0)

    @unittest.skipUnless(shutil.which("go"), "没有 go 工具链")
    def test_generated_go_compiles_vets_and_behaves(self):
        gomod = self.out / "gomod"
        self.assertEqual(self.gen("--go-out", str(gomod)), 0)
        shutil.copy(HERE / "testdata" / "go_behavior_test.go.txt", gomod / "behavior_test.go")
        (gomod / "go.mod").write_text("module probe\n\ngo 1.21\n", encoding="utf-8")

        def run(*cmd):
            r = subprocess.run(cmd, cwd=gomod, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, f"{' '.join(cmd)}\n{r.stdout}{r.stderr}")
            return r.stdout

        self.assertEqual(run("gofmt", "-l", ".").strip(), "", "生成的 Go 代码没有 gofmt")
        run("go", "vet", "./...")
        run("go", "test", "./...")


def deep_merge(base, over):
    """与运行时 Config::Load 一致：map 递归合并，标量与列表整体替换。"""
    if isinstance(base, dict) and isinstance(over, dict):
        out = dict(base)
        for k, v in over.items():
            out[k] = deep_merge(base[k], v) if k in base else v
        return out
    return over


def diff_paths(defs: dict, node: dict, doc, path: str = "") -> tuple:
    """schema 与 yaml 文档逐字段对照，返回 (schema 有而 yaml 没有, yaml 有而 schema 没有)。

    列表元素、map 的值也逐个检查，路径形如 redis.nodes[0].port。
    optional 字段（anyOf 带 null）可以不写，写了就照常检查。
    """
    if "$ref" in node:
        return diff_paths(defs, defs[node["$ref"].split("/")[-1]], doc, path)
    if "allOf" in node:
        return diff_paths(defs, node["allOf"][0], doc, path)
    if "anyOf" in node:
        return diff_paths(defs, node["anyOf"][0], doc, path)

    missing, extra = [], []
    if "properties" in node:
        if not isinstance(doc, dict):
            return missing, extra
        for key, sub in node["properties"].items():
            here = f"{path}.{key}" if path else key
            if key not in doc:
                if "anyOf" not in sub:
                    missing.append(here)
            else:
                m, e = diff_paths(defs, sub, doc[key], here)
                missing += m
                extra += e
        extra += [f"{path}.{k}" if path else k for k in doc if k not in node["properties"]]
    elif node.get("type") == "array" and isinstance(doc, list):
        for i, item in enumerate(doc):
            m, e = diff_paths(defs, node["items"], item, f"{path}[{i}]")
            missing += m
            extra += e
    elif isinstance(node.get("additionalProperties"), dict) and isinstance(doc, dict):  # map
        for k, item in doc.items():
            m, e = diff_paths(defs, node["additionalProperties"], item, f"{path}.{k}")
            missing += m
            extra += e
    return missing, extra


def resolve(defs: dict, node: dict) -> dict:
    """顺着 $ref / allOf / anyOf（optional）找到真正的类型节点。"""
    while True:
        if "$ref" in node:
            node = defs[node["$ref"].split("/")[-1]]
        elif "allOf" in node:
            node = node["allOf"][0]
        elif "anyOf" in node:
            node = node["anyOf"][0]
        else:
            return node


def env_value_paths(defs: dict, node: dict, path: str = "") -> set:
    """schema 里标了 env_value 的字段路径（叶子字段，不再往 list / map 里走）。"""
    out = set()
    for key, sub in resolve(defs, node).get("properties", {}).items():
        here = f"{path}.{key}" if path else key
        if sub.get("x-env-value"):
            out.add(here)
        else:
            out |= env_value_paths(defs, sub, here)
    return out


def written_leaf_paths(defs: dict, node: dict, doc, path: str = "") -> set:
    """yaml 文档里实际写了的字段路径，粒度与 env_value_paths 一致（遇到标量 / list / map 就停）。"""
    props = resolve(defs, node).get("properties")
    if props is None or not isinstance(doc, dict):
        return {path}
    out = set()
    for key, value in doc.items():
        here = f"{path}.{key}" if path else key
        out |= written_leaf_paths(defs, props[key], value, here) if key in props else {here}
    return out


class ShippedConfigTest(unittest.TestCase):
    """仓库里真实的 config/common.yaml + config/env/*.yaml 要写全量。"""

    def load_shipped(self):
        out = Path(tempfile.mkdtemp(prefix="configgen_full_"))
        self.addCleanup(shutil.rmtree, out, True)
        self.assertEqual(configgen.main(["--schema-dir", str(OPTIONS_DIR), "--jsonschema-out", str(out)]), 0)
        doc = json.loads((out / "common_config.schema.json").read_text(encoding="utf-8"))
        defs, root = doc["definitions"], doc["definitions"][doc["$ref"].split("/")[-1]]
        base = yaml.safe_load((CONFIG_DIR / "common.yaml").read_text(encoding="utf-8"))
        envs = {f.stem: yaml.safe_load(f.read_text(encoding="utf-8")) for f in sorted((CONFIG_DIR / "env").glob("*.yaml"))}
        self.assertTrue(envs)
        return defs, root, base, envs

    def test_shipped_config_is_full(self):
        defs, root, base, envs = self.load_shipped()
        # common.yaml 自己就是全量：schema 的每个字段（含 env_value 的）都有，也没有 schema 之外的键
        missing, extra = diff_paths(defs, root, base)
        self.assertEqual(missing, [], "common.yaml：schema 有、yaml 没写（取默认值的、env 的也要写）")
        self.assertEqual(extra, [], "common.yaml：yaml 有、schema 没有（拼错或 schema 已删）")
        for name, env in envs.items():
            missing, extra = diff_paths(defs, root, deep_merge(base, env))
            self.assertEqual(missing, [], f"common.yaml + {name}.yaml：schema 有、yaml 没写")
            self.assertEqual(extra, [], f"common.yaml + {name}.yaml：yaml 有、schema 没有")

    def test_env_files_override_exactly_the_env_value_fields(self):
        defs, root, base, envs = self.load_shipped()
        allowed = env_value_paths(defs, root)
        self.assertTrue(allowed, "schema 里没有标 env_value 的字段")
        self.assertEqual(sorted(allowed - written_leaf_paths(defs, root, base)), [],
                         "common.yaml 里没写这些 env_value 字段：env 要覆盖的字段，common.yaml 里也要有")
        for name, env in envs.items():
            written = written_leaf_paths(defs, root, env)
            self.assertEqual(sorted(written - allowed), [],
                             f"{name}.yaml 覆盖了没标 env_value 的字段：先在 schema 里标 env_value，或去掉它")
            self.assertEqual(sorted(allowed - written), [],
                             f"{name}.yaml 没写这些 env_value 字段：每个环境都要写出来，不能靠继承 common.yaml")

    def test_env_value_helpers(self):
        defs = {
            "A": {"properties": {"x": {"type": "integer", "x-env-value": True}, "n": {"$ref": "#/definitions/B"},
                                 "l": {"type": "array", "items": {"$ref": "#/definitions/B"}, "x-env-value": True}}},
            "B": {"properties": {"y": {"allOf": [{"$ref": "#/definitions/E"}], "x-env-value": True}, "z": {"type": "integer"}}},
            "E": {"enum": ["a"]},
        }
        a = defs["A"]
        self.assertEqual(env_value_paths(defs, a), {"x", "n.y", "l"})
        self.assertEqual(written_leaf_paths(defs, a, {"x": 1, "n": {"y": "a"}, "l": [{"y": "a"}]}), {"x", "n.y", "l"})
        self.assertEqual(written_leaf_paths(defs, a, {"n": {"z": 1}, "oops": 2}), {"n.z", "oops"})

    def test_diff_paths_both_directions(self):
        defs = {
            "A": {"properties": {"x": {"type": "integer"}, "b": {"$ref": "#/definitions/B"},
                                 "o": {"anyOf": [{"$ref": "#/definitions/B"}, {"type": "null"}]},
                                 "l": {"type": "array", "items": {"$ref": "#/definitions/B"}},
                                 "m": {"type": "object", "additionalProperties": {"$ref": "#/definitions/B"}}}},
            "B": {"properties": {"y": {"type": "integer"}, "z": {"type": "integer"}}},
        }
        a, b_ok = defs["A"], {"y": 1, "z": 2}
        full = {"x": 1, "b": b_ok, "l": [], "m": {}}
        self.assertEqual(diff_paths(defs, a, full), ([], []))
        self.assertEqual(diff_paths(defs, a, {**full, "o": b_ok}), ([], []))  # optional 写了也照常检查
        # schema 有、yaml 没有（optional 的 o 不算缺）
        self.assertEqual(diff_paths(defs, a, {"b": {"y": 1}, "l": [], "m": {}}), (["x", "b.z"], []))
        # yaml 有、schema 没有：顶层、嵌套、optional 内部
        self.assertEqual(diff_paths(defs, a, {**full, "extra": 1, "b": {**b_ok, "w": 1}, "o": {**b_ok, "q": 1}}),
                         ([], ["b.w", "o.q", "extra"]))
        # 列表元素、map 的值也逐个查，两个方向都是
        self.assertEqual(diff_paths(defs, a, {**full, "l": [b_ok, {"y": 1, "typo": 2}], "m": {"k": {"z": 1}}}),
                         (["l[1].z", "m.k.y"], ["l[1].typo"]))

    def test_deep_merge_matches_runtime_rules(self):
        self.assertEqual(deep_merge({"a": {"x": 1, "y": 2}, "l": [1, 2]}, {"a": {"y": 3}, "l": [9]}),
                         {"a": {"x": 1, "y": 3}, "l": [9]})


if __name__ == "__main__":
    unittest.main()
