#!/usr/bin/env python3
"""配置 schema 生成器：proto schema → C++ 头文件 / Go 源文件 / JSON Schema。

schema 用 proto 写（config/schema/*.proto，约束用 options.proto 里的自定义 option 表达），
配置数据文件是 yaml，生成的代码负责把 yaml 读成结构体。schema 是事实源，生成物不进版本库、不手改。
用法见 tools/configgen/CLAUDE.md。

  configgen.py --schema-dir config/schema --cpp-out build/gen --go-out src/go/gen/config \\
               --jsonschema-out build/gen
  configgen.py --schema-dir config/schema --check        # 只校验 schema，不生成

proto 由 protoc 解析（--descriptor_set_out），本脚本自带一个很小的 protobuf 解码器读它，
所以只依赖 protoc 本身：不需要 Python 的 protobuf 包，也没有 protoc 与库的版本匹配问题。
输出是确定的（没有时间戳、顺序固定），内容没变就不重写文件，不会让构建系统白白重编。
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


class SchemaError(Exception):
    """schema 本身有问题。message 已带 文件:行号。"""


# ---------------------------------------------------------------------------------------------
# 标量类型表：name → (C++, Go, JSON Schema, 下界, 上界)
# ---------------------------------------------------------------------------------------------

SCALARS = {
    "bool": ("bool", "bool", "boolean", None, None),
    "int32": ("std::int32_t", "int32", "integer", -(2**31), 2**31 - 1),
    "uint32": ("std::uint32_t", "uint32", "integer", 0, 2**32 - 1),
    "int64": ("std::int64_t", "int64", "integer", -(2**63), 2**63 - 1),
    "uint64": ("std::uint64_t", "uint64", "integer", 0, 2**64 - 1),
    "float": ("float", "float32", "number", None, None),
    "double": ("double", "float64", "number", None, None),
    "string": ("std::string", "string", "string", None, None),
}
INT_TYPES = {"int32", "uint32", "int64", "uint64"}
NUM_TYPES = INT_TYPES | {"float", "double"}

# protobuf FieldDescriptorProto.Type
T_DOUBLE, T_FLOAT, T_INT64, T_UINT64, T_INT32 = 1, 2, 3, 4, 5
T_BOOL, T_STRING, T_MESSAGE, T_UINT32, T_ENUM = 8, 9, 11, 13, 14
PROTO_SCALAR = {T_DOUBLE: "double", T_FLOAT: "float", T_INT64: "int64", T_UINT64: "uint64", T_INT32: "int32",
                T_BOOL: "bool", T_STRING: "string", T_UINT32: "uint32"}
PROTO_TYPE_NAME = {6: "fixed64", 7: "fixed32", 10: "group", 12: "bytes", 15: "sfixed32", 16: "sfixed64",
                   17: "sint32", 18: "sint64"}
LABEL_REPEATED = 3

# options.proto 里声明的自定义 option 编号。生成器启动时会拿 protoc 解析出的 options.proto 来核对，防止两边漂移。
OPTION_NUMBERS = {
    ("FieldOptions", "required"): 52001,
    ("FieldOptions", "secret"): 52002,
    ("FieldOptions", "reloadable"): 52003,
    ("FieldOptions", "default"): 52004,
    ("FieldOptions", "range"): 52005,
    ("FieldOptions", "min_count"): 52006,
    ("FieldOptions", "env_value"): 52007,
    ("MessageOptions", "root"): 52101,
}
OPTIONS_FILE = "options.proto"
OPTIONS_PACKAGE = "config"

CPP_KEYWORDS = {
    "alignas", "alignof", "and", "asm", "auto", "bool", "break", "case", "catch", "char", "class", "const",
    "constexpr", "continue", "decltype", "default", "delete", "do", "double", "else", "enum", "explicit", "export",
    "extern", "false", "float", "for", "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new",
    "noexcept", "not", "nullptr", "operator", "or", "private", "protected", "public", "register", "return", "short",
    "signed", "sizeof", "static", "struct", "switch", "template", "this", "throw", "true", "try", "typedef", "typeid",
    "typename", "union", "unsigned", "using", "virtual", "void", "volatile", "while", "xor",
}

FIELD_NAME_RE = re.compile(r"^[a-z][a-z0-9_]*$")
TYPE_NAME_RE = re.compile(r"^[A-Z][A-Za-z0-9]*$")
ENUM_REST_RE = re.compile(r"^[A-Z][A-Z0-9_]*$")


# ---------------------------------------------------------------------------------------------
# 一个很小的 protobuf 解码器：只读 protoc 输出的 FileDescriptorSet，不依赖 Python 的 protobuf 包
# ---------------------------------------------------------------------------------------------


def _varint(b: bytes, i: int):
    shift = result = 0
    while True:
        c = b[i]
        i += 1
        result |= (c & 0x7F) << shift
        if not c & 0x80:
            return result, i
        shift += 7


def _fields(b: bytes):
    """逐个产出 (字段号, wire 类型, 值)。varint 给 int，其余给 bytes。"""
    i = 0
    while i < len(b):
        key, i = _varint(b, i)
        no, wt = key >> 3, key & 7
        if wt == 0:
            v, i = _varint(b, i)
        elif wt == 1:
            v, i = b[i:i + 8], i + 8
        elif wt == 2:
            n, i = _varint(b, i)
            v, i = b[i:i + n], i + n
        elif wt == 5:
            v, i = b[i:i + 4], i + 4
        else:
            raise SchemaError(f"描述符里有不认识的 wire 类型 {wt}（protoc 输出损坏？）")
        yield no, wt, v


def _packed_varints(b: bytes) -> tuple:
    out, i = [], 0
    while i < len(b):
        v, i = _varint(b, i)
        out.append(v)
    return tuple(out)


def _s(b: bytes) -> str:
    return b.decode("utf-8")


@dataclasses.dataclass
class FieldD:
    name: str = ""
    number: int = 0
    label: int = 1
    type: int = 0
    type_name: str = ""
    extendee: str = ""
    options: bytes = b""
    oneof_index: "int | None" = None
    proto3_optional: bool = False


@dataclasses.dataclass
class MsgD:
    name: str = ""
    fields: list = dataclasses.field(default_factory=list)
    nested: list = dataclasses.field(default_factory=list)
    nested_enums: int = 0
    oneofs: int = 0
    options: bytes = b""
    map_entry: bool = False


@dataclasses.dataclass
class EnumD:
    name: str = ""
    values: list = dataclasses.field(default_factory=list)  # [(名字, 编号)]


@dataclasses.dataclass
class FileD:
    name: str = ""
    package: str = ""
    deps: list = dataclasses.field(default_factory=list)
    msgs: list = dataclasses.field(default_factory=list)
    enums: list = dataclasses.field(default_factory=list)
    services: int = 0
    extensions: list = dataclasses.field(default_factory=list)  # [FieldD]
    syntax: str = "proto2"
    locations: dict = dataclasses.field(default_factory=dict)  # 路径 → (行号 1 起, 前导注释)


def _parse_field(b: bytes) -> FieldD:
    f = FieldD()
    for no, _, v in _fields(b):
        if no == 1: f.name = _s(v)
        elif no == 2: f.extendee = _s(v)
        elif no == 3: f.number = v
        elif no == 4: f.label = v
        elif no == 5: f.type = v
        elif no == 6: f.type_name = _s(v)
        elif no == 8: f.options = v
        elif no == 9: f.oneof_index = v
        elif no == 17: f.proto3_optional = bool(v)
    return f


def _parse_message(b: bytes) -> MsgD:
    m = MsgD()
    for no, _, v in _fields(b):
        if no == 1: m.name = _s(v)
        elif no == 2: m.fields.append(_parse_field(v))
        elif no == 3: m.nested.append(_parse_message(v))
        elif no == 4: m.nested_enums += 1
        elif no == 7:
            m.options = v
            for ono, _, ov in _fields(v):
                if ono == 7:  # MessageOptions.map_entry
                    m.map_entry = bool(ov)
        elif no == 8: m.oneofs += 1
    return m


def _parse_enum(b: bytes) -> EnumD:
    e = EnumD()
    for no, _, v in _fields(b):
        if no == 1:
            e.name = _s(v)
        elif no == 2:
            name, number = "", 0
            for vno, _, vv in _fields(v):
                if vno == 1: name = _s(vv)
                elif vno == 2: number = vv
            e.values.append((name, number))
    return e


def _parse_file(b: bytes) -> FileD:
    f = FileD()
    for no, _, v in _fields(b):
        if no == 1: f.name = _s(v)
        elif no == 2: f.package = _s(v)
        elif no == 3: f.deps.append(_s(v))
        elif no == 4: f.msgs.append(_parse_message(v))
        elif no == 5: f.enums.append(_parse_enum(v))
        elif no == 6: f.services += 1
        elif no == 7: f.extensions.append(_parse_field(v))
        elif no == 12: f.syntax = _s(v)
        elif no == 9:  # SourceCodeInfo
            for lno, _, lv in _fields(v):
                if lno != 1:
                    continue
                path, span, comment = (), (0,), ""
                for pno, _, pv in _fields(lv):
                    if pno == 1: path = _packed_varints(pv)
                    elif pno == 2: span = _packed_varints(pv)
                    elif pno == 3: comment = _s(pv)
                f.locations.setdefault(path, (span[0] + 1, comment))
    return f


def parse_descriptor_set(b: bytes) -> list:
    return [_parse_file(v) for no, _, v in _fields(b) if no == 1]


def _doc(comment: str) -> str:
    """proto 的前导注释：每行开头的一个空格去掉。"""
    lines = [ln[1:] if ln.startswith(" ") else ln for ln in comment.splitlines()]
    return "\n".join(lines).strip()


# ---------------------------------------------------------------------------------------------
# 模型
# ---------------------------------------------------------------------------------------------


@dataclasses.dataclass
class Type:
    kind: str  # scalar | enum | message | list | map | optional
    name: str = ""
    ref: object = None
    elem: "Type | None" = None


@dataclasses.dataclass
class Enum:
    name: str
    values: list  # yaml 里写的名字，已去掉前缀、转小写；不含 UNSPECIFIED
    doc: str
    owner: "Schema"


@dataclasses.dataclass
class Field:
    key: str
    type: Type
    required: bool
    has_default: bool
    default: object
    secret: bool
    reloadable: bool
    range: "tuple | None"
    min_count: "int | None"
    env_value: bool
    doc: str


@dataclasses.dataclass
class Message:
    name: str
    doc: str
    root: bool
    owner: "Schema"
    fields: list = dataclasses.field(default_factory=list)


@dataclasses.dataclass
class Schema:
    name: str  # 文件名（不带 .proto）：生成文件名
    package: str  # proto package：C++ 命名空间
    path: Path
    imports: list = dataclasses.field(default_factory=list)
    enums: dict = dataclasses.field(default_factory=dict)
    messages: dict = dataclasses.field(default_factory=dict)


# ---------------------------------------------------------------------------------------------
# 调用 protoc
# ---------------------------------------------------------------------------------------------


def _is_win_exe_on_unix(protoc: str) -> bool:
    return protoc.lower().endswith(".exe") and sys.platform != "win32"


def find_protoc(explicit: "str | None" = None) -> str:
    for cand in (explicit, os.environ.get("PROTOC")):
        if cand:
            if not (shutil.which(cand) or Path(cand).exists()):
                raise SchemaError(f"指定的 protoc 不存在: {cand}")
            return cand
    for name in ("protoc", "protoc.exe"):  # WSL 里 Windows 装的 protoc.exe 也在 PATH 上
        found = shutil.which(name)
        if found:
            return found
    raise SchemaError("找不到 protoc：装好后加进 PATH，或用 --protoc / 环境变量 PROTOC 指定"
                      "（scripts/setup_windows_env.ps1 会检测并安装）")


def _native(protoc: str, p: Path) -> str:
    """传给 protoc 的路径：Windows 的 protoc.exe 在 WSL 里要转成盘符路径。"""
    if _is_win_exe_on_unix(protoc):
        r = subprocess.run(["wslpath", "-w", str(p)], capture_output=True, text=True)
        if r.returncode == 0:
            return r.stdout.strip()
    return str(p)


def make_scratch_dir(protoc: str) -> Path:
    """暂存目录必须是纯 ASCII 路径，且 protoc 看得见。

    Windows 的 protoc.exe 按 ANSI 代码页读命令行，路径里有中文（比如仓库在 "源代码" 下）会乱码；
    所以不直接在源码目录上跑 protoc，而是把 schema 复制到这里再跑。
    """
    base = None
    if _is_win_exe_on_unix(protoc):  # WSL：用 Windows 的 %TEMP%，protoc.exe 才看得见
        r = subprocess.run(["cmd.exe", "/c", "echo %TEMP%"], capture_output=True, text=True)
        if r.returncode == 0:
            u = subprocess.run(["wslpath", "-u", r.stdout.strip()], capture_output=True, text=True)
            if u.returncode == 0 and Path(u.stdout.strip()).is_dir():
                base = u.stdout.strip()
    return Path(tempfile.mkdtemp(prefix="configgen_", dir=base))


def _copy_protos(src: Path, dst: Path) -> None:
    for p in sorted(src.rglob("*.proto")):
        out = dst / p.relative_to(src)
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(p, out)


def run_protoc(protoc: str, schema_dir: Path, includes: list, names: list) -> list:
    """把 schema 目录与 include 目录复制到暂存目录，跑 protoc，返回全部 FileD（含 import 进来的）。"""
    scratch = make_scratch_dir(protoc)
    try:
        stage_schema = scratch / "schema"
        _copy_protos(schema_dir, stage_schema)
        inc_dirs, seen = [stage_schema], {schema_dir.resolve()}
        for i, inc in enumerate(includes):
            inc = Path(inc)
            if not inc.is_dir():
                raise SchemaError(f"include 目录不存在: {inc}")
            if inc.resolve() in seen:
                continue
            seen.add(inc.resolve())
            dst = scratch / f"inc{i}"
            _copy_protos(inc, dst)
            inc_dirs.append(dst)

        out = scratch / "d.pb"
        cmd = [protoc, *[f"-I{_native(protoc, d)}" for d in inc_dirs], f"--descriptor_set_out={_native(protoc, out)}",
               "--include_imports", "--include_source_info",
               *[_native(protoc, stage_schema / f"{n}.proto") for n in names]]
        r = subprocess.run(cmd, capture_output=True)
        if r.returncode != 0 or not out.exists():
            err = (r.stderr or r.stdout).decode("utf-8", "replace").strip()
            # 暂存路径对用户没有意义：换回真实的 schema 路径
            for form in (_native(protoc, stage_schema), str(stage_schema), _native(protoc, stage_schema).replace("\\", "/")):
                err = err.replace(form + "\\", str(schema_dir) + "/").replace(form + "/", str(schema_dir) + "/")
            raise SchemaError(f"protoc 解析失败:\n{err}")
        return parse_descriptor_set(out.read_bytes())
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


# ---------------------------------------------------------------------------------------------
# 从描述符构建并校验模型
# ---------------------------------------------------------------------------------------------


class _File:
    """一个 proto 文件，出错时指到具体哪一行。"""

    def __init__(self, d: FileD, real_path: Path):
        self.d = d
        self.path = real_path

    def line(self, loc: tuple) -> int:
        while loc and loc not in self.d.locations:
            loc = loc[:-1]
        return self.d.locations.get(loc, (1, ""))[0]

    def doc(self, loc: tuple) -> str:
        return _doc(self.d.locations.get(loc, (0, ""))[1])

    def fail(self, loc: tuple, what: str, msg: str):
        raise SchemaError(f"{self.path}:{self.line(loc)}: {what + ': ' if what else ''}{msg}")


def _check_options_file(files: dict) -> None:
    """options.proto 里的 option 编号必须和本脚本里的表一致。"""
    d = files.get(OPTIONS_FILE)
    if d is None:
        return  # 没有 schema 用到 option，没什么可核对的
    declared = {}
    for e in d.extensions:
        declared[(e.extendee.rsplit(".", 1)[-1], e.name)] = e.number
    if d.package != OPTIONS_PACKAGE or declared != OPTION_NUMBERS:
        raise SchemaError(f"{OPTIONS_FILE}:1: 与 configgen.py 里的 OPTION_NUMBERS 不一致，两边要一起改。\n"
                          f"  options.proto: {sorted(declared.items())}\n  configgen.py:  {sorted(OPTION_NUMBERS.items())}")


def _decode_field_options(b: bytes, fail) -> dict:
    o = {"required": False, "secret": False, "reloadable": False, "default": None, "range": None, "min_count": None,
         "env_value": False}
    for no, _, v in _fields(b):
        if no == 52001: o["required"] = bool(v)
        elif no == 52002: o["secret"] = bool(v)
        elif no == 52003: o["reloadable"] = bool(v)
        elif no == 52004: o["default"] = _s(v)
        elif no == 52005:
            lo = hi = 0.0  # proto3 的 0 不序列化，缺省就是 0
            for rno, _, rv in _fields(v):
                if rno == 1: lo = struct.unpack("<d", rv)[0]
                elif rno == 2: hi = struct.unpack("<d", rv)[0]
            o["range"] = (lo, hi)
        elif no == 52006: o["min_count"] = v
        elif no == 52007: o["env_value"] = bool(v)
        elif no >= 1000:
            fail(f"不认识的自定义 option（编号 {no}）：配置 schema 只支持 options.proto 里 config 的 option")
    return o


def _decode_message_options(b: bytes, fail) -> dict:
    o = {"root": False}
    for no, _, v in _fields(b):
        if no == 52101: o["root"] = bool(v)
        elif no >= 1000:
            fail(f"不认识的自定义 option（编号 {no}）：配置 schema 只支持 options.proto 里 config 的 option")
    return o


def build_schemas(descs: list, schema_dir: Path, names: list) -> list:
    files = {d.name: d for d in descs}
    _check_options_file(files)

    # 本目录的 schema 文件（不含 options.proto）。import 进来的 google/* 与 options.proto 不生成。
    schema_names = sorted(n[:-6] for n in files if n.endswith(".proto") and "/" not in n and n != OPTIONS_FILE
                          and (schema_dir / n).exists())
    wrap = {n: _File(files[f"{n}.proto"], schema_dir / f"{n}.proto") for n in schema_names}

    schemas: dict[str, Schema] = {}
    symbols: dict[str, object] = {}  # ".pkg.Name" → Enum | Message

    # 第一遍：登记所有文件里的类型名（跨文件引用要用）
    for n in schema_names:
        f = wrap[n]
        d = f.d
        if d.syntax != "proto3":
            f.fail((12,), "", f"必须用 syntax = \"proto3\"（现在是 {d.syntax!r}）：proto2 的 required / default 与本项目的 option 语义冲突")
        if not d.package:
            f.fail((2,), "", "必须声明 package（它就是生成的 C++ 命名空间）")
        if d.services:
            f.fail((6, 0), "", "配置 schema 里不能有 service")
        if any(e.extendee for e in d.extensions):
            f.fail((7, 0), "", "配置 schema 里不能有 extend：自定义 option 只放在 options.proto")
        for dep in d.deps:
            if dep == OPTIONS_FILE or dep.startswith("google/protobuf/"):
                continue
            if dep[:-6] not in schema_names:
                f.fail((3,), "", f"只能 import 同目录下的 schema，'{dep}' 不在 {schema_dir}")
        sc = Schema(name=n, package=d.package, path=f.path)
        schemas[n] = sc

        for ei, e in enumerate(d.enums):
            loc = (5, ei)
            if not TYPE_NAME_RE.match(e.name):
                f.fail(loc, e.name, f"枚举名 '{e.name}' 不合法，要求 大写字母开头的 [A-Za-z0-9]")
            enum = _build_enum(f, loc, e, sc)
            sc.enums[e.name] = enum
            symbols[f".{d.package}.{e.name}"] = enum

        for mi, m in enumerate(d.msgs):
            loc = (4, mi)
            if not TYPE_NAME_RE.match(m.name):
                f.fail(loc, m.name, f"类型名 '{m.name}' 不合法，要求 大写字母开头的 [A-Za-z0-9]")
            real_nested = [x for x in m.nested if not x.map_entry]
            if real_nested or m.nested_enums:
                f.fail(loc, m.name, "不支持嵌套的 message / enum 定义，请放到文件顶层")
            mopt = _decode_message_options(m.options, lambda msg, loc=loc, nm=m.name: f.fail(loc, nm, msg))
            obj = Message(m.name, f.doc(loc), mopt["root"], sc)
            sc.messages[m.name] = obj
            symbols[f".{d.package}.{m.name}"] = obj

    # imports（用的是直接 import 的 schema）
    for n in schema_names:
        for dep in wrap[n].d.deps:
            if dep[:-6] in schemas:
                schemas[n].imports.append(schemas[dep[:-6]])

    # 第二遍：字段
    for n in schema_names:
        f = wrap[n]
        for mi, m in enumerate(f.d.msgs):
            obj = schemas[n].messages.get(m.name)
            if obj is not None:
                _build_fields(f, (4, mi), m, obj, symbols)
        _check_cycles(f, schemas[n])

    missing = [n for n in names if n not in schemas]
    if missing:
        raise SchemaError(f"找不到 schema: {', '.join(missing)}（{schema_dir}/<名>.proto）")
    return [schemas[n] for n in (names or schema_names)]


def _build_enum(f: _File, loc: tuple, e: EnumD, sc: Schema) -> Enum:
    prefix = snake(e.name).upper() + "_"
    if not e.values or e.values[0] != (prefix + "UNSPECIFIED", 0):
        f.fail(loc, e.name, f"第一个枚举值必须是 {prefix}UNSPECIFIED = 0（proto3 的要求，也表示“没设置”）")
    values, seen = [], {}
    for vi, (vname, number) in enumerate(e.values[1:], start=1):
        vloc = loc + (2, vi)
        if not vname.startswith(prefix) or not ENUM_REST_RE.match(vname[len(prefix):]):
            f.fail(vloc, e.name, f"枚举值 '{vname}' 必须是 {prefix}<大写名字>（proto 枚举值在包内是全局的，所以带枚举名前缀）")
        if number != vi:
            f.fail(vloc, e.name, f"枚举值 '{vname}' 的编号是 {number}，要求从 0 起连续（这里应是 {vi}）")
        yaml_name = vname[len(prefix):].lower()
        pas = pascal(yaml_name)
        if pas in seen:
            f.fail(vloc, e.name, f"枚举值 '{vname}' 与 '{seen[pas]}' 转成标识符后都是 '{pas}'")
        seen[pas] = vname
        values.append(yaml_name)
    if not values:
        f.fail(loc, e.name, "枚举除了 UNSPECIFIED 之外至少要有一个值")
    return Enum(e.name, values, f.doc(loc), sc)


def _resolve(f: _File, loc: tuple, what: str, fd: FieldD, symbols: dict, owner_msg: MsgD) -> Type:
    if fd.type in PROTO_SCALAR:
        return Type("scalar", name=PROTO_SCALAR[fd.type])
    if fd.type in (T_ENUM, T_MESSAGE):
        obj = symbols.get(fd.type_name)
        if obj is None:
            f.fail(loc, what, f"引用了不是 schema 里声明的类型 '{fd.type_name}'")
        if isinstance(obj, Enum):
            return Type("enum", name=obj.name, ref=obj)
        return Type("message", name=obj.name, ref=obj)
    f.fail(loc, what, f"不支持的字段类型 {PROTO_TYPE_NAME.get(fd.type, fd.type)}"
                      f"（支持: bool int32 uint32 int64 uint64 float double string、枚举、message）")


def _build_fields(f: _File, loc: tuple, m: MsgD, msg: Message, symbols: dict) -> None:
    if not m.fields:
        f.fail(loc, m.name, "message 至少要有一个字段")
    real_oneof = any(fd.oneof_index is not None and not fd.proto3_optional for fd in m.fields)
    if real_oneof:
        f.fail(loc, m.name, "不支持 oneof")
    entries = {x.name: x for x in m.nested if x.map_entry}

    cpp_names: dict = {}
    go_names: dict = {}
    for fi, fd in enumerate(m.fields):
        floc = loc + (2, fi)
        what = f"{m.name}.{fd.name}"
        if not FIELD_NAME_RE.match(fd.name):
            f.fail(floc, what, f"字段名 '{fd.name}' 不合法，要求 小写字母开头的 [a-z0-9_]")
        cn, gn = camel(fd.name), pascal(fd.name)
        if cn in CPP_KEYWORDS:
            f.fail(floc, what, f"字段名 '{fd.name}' 在 C++ 里是关键字 '{cn}'，换一个")
        for table, nme, label in ((cpp_names, cn, "C++"), (go_names, gn, "Go")):
            if nme in table:
                f.fail(floc, what, f"字段名 '{fd.name}' 与 '{table[nme]}' 转成 {label} 标识符后都是 '{nme}'")
            table[nme] = fd.name

        # 类型：list / map / optional / 直接
        entry = entries.get(fd.type_name.rsplit(".", 1)[-1]) if fd.type == T_MESSAGE else None
        if fd.label == LABEL_REPEATED and entry is not None:
            key = next(x for x in entry.fields if x.name == "key")
            val = next(x for x in entry.fields if x.name == "value")
            if key.type != T_STRING:
                f.fail(floc, what, f"map 的键只支持 string（现在是 {PROTO_TYPE_NAME.get(key.type, key.type)}）")
            t = Type("map", elem=_resolve(f, floc, what, val, symbols, m))
        elif fd.label == LABEL_REPEATED:
            t = Type("list", elem=_resolve(f, floc, what, fd, symbols, m))
        elif fd.proto3_optional:
            t = Type("optional", elem=_resolve(f, floc, what, fd, symbols, m))
        else:
            t = _resolve(f, floc, what, fd, symbols, m)

        opts = _decode_field_options(fd.options, lambda msg_, floc=floc, what=what: f.fail(floc, what, msg_))
        if opts["secret"] and opts["reloadable"]:
            f.fail(floc, what, "secret 与 reloadable 不能同时为 true（密钥换了要重连，不能只改个值）")

        has_default, default = opts["default"] is not None, None
        if has_default:
            if opts["required"]:
                f.fail(floc, what, "required 与 default 矛盾：必填的字段不会用到默认值")
            if opts["secret"]:
                f.fail(floc, what, "secret 字段不能有 default：密钥不该写进仓库")
            default = _parse_default(f, floc, what, t, opts["default"])

        rng = None
        if opts["range"] is not None:
            rng = _check_range(f, floc, what, t, opts["range"])
            if has_default and not (rng[0] <= default <= rng[1]):
                f.fail(floc, what, f"默认值 {default} 不在 range {list(rng)} 内")

        min_count = opts["min_count"]
        if min_count is not None:
            if t.kind not in ("list", "map"):
                f.fail(floc, what, "min_count 只能用在 repeated / map 字段上")
            if min_count < 1:
                f.fail(floc, what, "min_count 至少是 1（0 等于没写）")

        if opts["env_value"] and (t.kind == "message" or (t.kind == "optional" and t.elem.kind == "message")):
            f.fail(floc, what, "env_value 要标在叶子字段（标量 / 枚举 / list / map）上：message 字段整段替换不是合并，"
                               "env 文件该覆盖它里面的具体字段")

        msg.fields.append(Field(fd.name, t, opts["required"], has_default, default, opts["secret"], opts["reloadable"],
                                rng, min_count, opts["env_value"], f.doc(floc)))


def _parse_default(f: _File, loc: tuple, what: str, t: Type, text: str):
    if t.kind == "enum":
        if text not in t.ref.values:
            f.fail(loc, what, f"default '{text}' 不是枚举 {t.name} 的取值（{' | '.join(t.ref.values)}）")
        return text
    if t.kind != "scalar":
        f.fail(loc, what, "只有标量、字符串、枚举字段可以有 default（repeated / map / optional / message 默认就是空）")
    n = t.name
    if n == "bool":
        if text not in ("true", "false"):
            f.fail(loc, what, f"default '{text}' 不是 bool（写 true / false）")
        return text == "true"
    if n in INT_TYPES:
        lo, hi = SCALARS[n][3], SCALARS[n][4]
        if not re.fullmatch(r"-?\d+", text) or not (lo <= int(text) <= hi):
            f.fail(loc, what, f"default '{text}' 不是 {n}（范围 [{lo}, {hi}]）")
        return int(text)
    if n in ("float", "double"):
        try:
            v = float(text)
        except ValueError:
            f.fail(loc, what, f"default '{text}' 不是数字")
        if v != v or v in (float("inf"), float("-inf")):
            f.fail(loc, what, f"default '{text}' 不是有限的数字")
        return v
    return text  # string


def _check_range(f: _File, loc: tuple, what: str, t: Type, r: tuple) -> tuple:
    if t.kind != "scalar" or t.name not in NUM_TYPES:
        f.fail(loc, what, "range 只能用在数值类型（整数、float、double）的字段上")
    lo, hi = r
    if lo > hi:
        f.fail(loc, what, f"range 的下界 {lo:g} 大于上界 {hi:g}")
    if t.name in INT_TYPES:
        if lo != int(lo) or hi != int(hi):
            f.fail(loc, what, "整数字段的 range 必须是整数")
        lo, hi = int(lo), int(hi)
        if max(abs(lo), abs(hi)) > 2**53:
            f.fail(loc, what, "range 的边界超过 2^53，option 里用 double 存会丢精度，请缩小范围")
        tlo, thi = SCALARS[t.name][3], SCALARS[t.name][4]
        if lo < tlo or hi > thi:
            f.fail(loc, what, f"range [{lo}, {hi}] 超出 {t.name} 的取值范围 [{tlo}, {thi}]")
    return (lo, hi)


def _check_cycles(f: _File, schema: Schema) -> None:
    def deps(t: Type):
        if t.kind == "message" and t.ref.owner is schema:
            yield t.ref.name
        if t.elem:
            yield from deps(t.elem)

    graph = {m.name: {d for fl in m.fields for d in deps(fl.type)} for m in schema.messages.values()}
    state: dict = {}

    def visit(n: str, trail: list):
        if state.get(n) == 2:
            return
        if state.get(n) == 1:
            f.fail((), n, "message 之间有循环引用: " + " -> ".join(trail[trail.index(n):] + [n]))
        state[n] = 1
        for d in sorted(graph[n]):
            visit(d, trail + [n])
        state[n] = 2

    for n in graph:
        visit(n, [])


def load_schemas(schema_dir, includes=(), protoc=None, names=None) -> list:
    schema_dir = Path(schema_dir)
    if not schema_dir.is_dir():
        raise SchemaError(f"schema 目录不存在: {schema_dir}")
    avail = sorted(p.stem for p in schema_dir.glob("*.proto") if p.name != OPTIONS_FILE)
    names = list(names) if names else avail
    if not names:
        raise SchemaError(f"{schema_dir} 下没有 *.proto schema（options.proto 不算）")
    exe = find_protoc(protoc)
    descs = run_protoc(exe, schema_dir, list(includes), [n for n in names if n in avail] or avail)
    return build_schemas(descs, schema_dir, names if names else [])


# ---------------------------------------------------------------------------------------------
# 命名
# ---------------------------------------------------------------------------------------------


def camel(s: str) -> str:
    parts = s.split("_")
    return parts[0] + "".join(p.capitalize() for p in parts[1:])


def pascal(s: str) -> str:
    return "".join(p.capitalize() for p in s.split("_"))


def snake(s: str) -> str:
    return re.sub(r"(?<!^)(?=[A-Z])", "_", s).lower()


def ordered_messages(schema: Schema) -> list:
    """被依赖的 message 排前面（C++ 需要先定义后使用）。同层级保持 schema 里的顺序。"""
    out, done = [], set()

    def deps(t: Type):
        if t.kind == "message" and t.ref.owner is schema:
            yield t.ref
        if t.elem:
            yield from deps(t.elem)

    def visit(m: Message):
        if m.name in done:
            return
        done.add(m.name)
        for f in m.fields:
            for d in deps(f.type):
                visit(d)
        out.append(m)

    for m in schema.messages.values():
        visit(m)
    return out


def comment_lines(doc: str, indent: str, prefix: str = "// ") -> list:
    return [f"{indent}{prefix}{line}".rstrip() for line in doc.strip().splitlines()] if doc.strip() else []


# ---------------------------------------------------------------------------------------------
# C++
# ---------------------------------------------------------------------------------------------


def cpp_ns(schema: Schema) -> str:
    return schema.package.replace(".", "::")


def cpp_full(ref) -> str:
    return f"::{cpp_ns(ref.owner)}::{ref.name}"


def cpp_type(t: Type, cur: Schema) -> str:
    k = t.kind
    if k == "scalar":
        return SCALARS[t.name][0]
    if k in ("enum", "message"):
        return t.ref.name if t.ref.owner is cur else cpp_full(t.ref)
    inner = cpp_type(t.elem, cur)
    return {"list": f"std::vector<{inner}>", "map": f"std::map<std::string, {inner}>",
            "optional": f"std::optional<{inner}>"}[k]


def cpp_str(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def cpp_num(t: Type, v) -> str:
    n = t.name
    if n in INT_TYPES:
        suffix = {"int32": "", "uint32": "U", "int64": "LL", "uint64": "ULL"}[n]
        return f"{v}{suffix}"
    return f"{float(v)!r}" + ("f" if n == "float" else "")


def cpp_default(f: Field, cur: Schema) -> str:
    t, v = f.type, f.default
    if t.kind == "enum":
        return f"{cpp_type(t, cur)}::{pascal(v)}"
    if t.name == "bool":
        return "true" if v else "false"
    if t.name == "string":
        return cpp_str(v)
    return cpp_num(t, v)


def emit_cpp(schema: Schema) -> str:
    ns = cpp_ns(schema)
    includes = ["<cstdint>", "<map>", "<optional>", "<string>", "<utility>", "<vector>"]
    local = ['"config.h"'] + [f'"{imp.name}_config.h"' for imp in schema.imports]
    o: list = [
        f"// Code generated by tools/configgen from {schema.path.name}. DO NOT EDIT.",
        "#pragma once",
        "",
        *[f"#include {i}" for i in includes],
        "",
        *[f"#include {i}" for i in local],
        "",
        f"namespace {ns}",
        "{",
        "",
    ]

    for e in schema.enums.values():
        o += comment_lines(e.doc, "")
        o.append(f"enum class {e.name} : std::uint8_t")
        o.append("{")
        o.append("    Unspecified, // 没设置；yaml 里不能写它")
        o += [f"    {pascal(v)}," for v in e.values]
        o += ["};", ""]

    for m in ordered_messages(schema):
        o += comment_lines(m.doc, "")
        o.append(f"struct {m.name}")
        o.append("{")
        for f in m.fields:
            o += comment_lines(f.doc, "    ")
            decl = f"    {cpp_type(f.type, schema)} {camel(f.key)}"
            o.append(decl + (f" = {cpp_default(f, schema)};" if f.has_default else "{};"))
        o += ["};", ""]

    o += [f"}} // namespace {ns}", "", "namespace breeze", "{", ""]

    for e in schema.enums.values():
        full = cpp_full(e)
        o += [
            "template <>",
            f"struct ConfigTraits<{full}> : EnumTraits<{full}>",
            "{",
            f"    static const std::vector<std::pair<std::string, {full}>>& Table()",
            "    {",
            f"        static const std::vector<std::pair<std::string, {full}>> table = {{",
            *[f'            {{"{v}", {full}::{pascal(v)}}},' for v in e.values],
            "        };",
            "        return table;",
            "    }",
            "};",
            "",
        ]

    for m in ordered_messages(schema):
        full = cpp_full(m)
        o += ["template <>", f"struct ConfigTraits<{full}> : StructTraits<{full}>", "{",
              f"    static FieldTable<{full}> MakeFields()", "    {", "        return {"]
        for f in m.fields:
            chain = f'Field("{f.key}", &{full}::{camel(f.key)})'
            if f.required:
                chain += ".Required()"
            if f.range:
                chain += f".Range({cpp_num(f.type, f.range[0])}, {cpp_num(f.type, f.range[1])})"
            if f.min_count:
                chain += (f'.Check([](const auto& v) -> std::string {{ return v.size() < {f.min_count} '
                          f'? "至少需要 {f.min_count} 个元素" : ""; }})')
            if f.secret:
                chain += ".Secret()"
            if f.reloadable:
                chain += ".Reloadable()"
            o.append(f"            {chain},")
        o += ["        };", "    }", "};", ""]

    o.append("} // namespace breeze")
    return "\n".join(o) + "\n"


# ---------------------------------------------------------------------------------------------
# Go
# ---------------------------------------------------------------------------------------------


def go_type(t: Type) -> str:
    k = t.kind
    if k == "scalar":
        return SCALARS[t.name][1]
    if k in ("enum", "message"):
        return t.name
    inner = go_type(t.elem)
    return {"list": f"[]{inner}", "map": f"map[string]{inner}", "optional": f"*{inner}"}[k]


def go_str(s: str) -> str:
    return json.dumps(s, ensure_ascii=False)


def go_default(f: Field) -> str:
    t, v = f.type, f.default
    if t.kind == "enum":
        return f"{t.name}{pascal(v)}"
    if t.name == "bool":
        return "true" if v else "false"
    if t.name == "string":
        return go_str(v)
    return repr(float(v)) if t.name in ("float", "double") else str(v)


def go_zero(t: Type) -> str:
    if t.kind == "scalar":
        return {"bool": "false", "string": '""'}.get(t.name, "0")
    if t.kind == "enum":
        return '""'
    if t.kind == "message":
        return f"{t.name}{{}}"
    return "nil" if t.kind in ("list", "map", "optional") else f"*new({go_type(t)})"


class _GoFile:
    def __init__(self):
        self.uses_fmt = False
        self.uses_reflect = False


def _go_validate(o: list, g: _GoFile, expr: str, t: Type, path: str, ind: str, depth: int, rng=None) -> None:
    k = t.kind
    if k == "scalar" and rng is not None:
        g.uses_fmt = True
        lo, hi = rng
        o.append(f"{ind}if {expr} < {lo} || {expr} > {hi} {{")
        o.append(f'{ind}\treturn fmt.Errorf("%s: %v 超出范围 [{lo}, {hi}]", {path}, {expr})')
        o.append(f"{ind}}}")
    elif k == "enum":
        g.uses_fmt = True
        o.append(f"{ind}if !{expr}.Valid() {{")
        o.append(f'{ind}\treturn fmt.Errorf("%s: 非法的 {t.name} 取值 %q", {path}, string({expr}))')
        o.append(f"{ind}}}")
    elif k == "message":
        g.uses_fmt = True
        o.append(f"{ind}if err := {expr}.Validate(); err != nil {{")
        o.append(f'{ind}\treturn fmt.Errorf("%s: %w", {path}, err)')
        o.append(f"{ind}}}")
    elif k == "list" and _needs_validation(t.elem):
        g.uses_fmt = True
        i = f"i{depth}"
        o.append(f"{ind}for {i} := range {expr} {{")
        _go_validate(o, g, f"{expr}[{i}]", t.elem, f'{path} + fmt.Sprintf("[%d]", {i})', ind + "\t", depth + 1)
        o.append(f"{ind}}}")
    elif k == "map" and _needs_validation(t.elem):
        g.uses_fmt = True
        kk, vv = f"k{depth}", f"v{depth}"
        o.append(f"{ind}for {kk}, {vv} := range {expr} {{")
        _go_validate(o, g, vv, t.elem, f'{path} + "." + {kk}', ind + "\t", depth + 1)
        o.append(f"{ind}}}")
    elif k == "optional" and _needs_validation(t.elem):
        o.append(f"{ind}if {expr} != nil {{")
        _go_validate(o, g, f"(*{expr})", t.elem, path, ind + "\t", depth + 1)
        o.append(f"{ind}}}")


def _needs_validation(t: Type) -> bool:
    return t.kind in ("enum", "message") or (t.elem is not None and _needs_validation(t.elem))


def _go_redact_expr(expr: str, t: Type, depth: int) -> "str | None":
    """返回计算打码后值的 Go 表达式；这个类型里没有 message 就返回 None（没什么可打码的）。"""
    if t.kind == "message":
        return f"{expr}.Redacted()"
    if t.kind == "list" and _contains_message(t.elem):
        inner = _go_redact_expr(f"in[i{depth}]", t.elem, depth + 1)
        ty = go_type(t)
        return (f"func(in {ty}) {ty} {{ if in == nil {{ return nil }}; out := make({ty}, len(in)); "
                f"for i{depth} := range in {{ out[i{depth}] = {inner} }}; return out }}({expr})")
    if t.kind == "map" and _contains_message(t.elem):
        inner = _go_redact_expr(f"v{depth}", t.elem, depth + 1)
        ty = go_type(t)
        return (f"func(in {ty}) {ty} {{ if in == nil {{ return nil }}; out := make({ty}, len(in)); "
                f"for k{depth}, v{depth} := range in {{ out[k{depth}] = {inner} }}; return out }}({expr})")
    if t.kind == "optional" and _contains_message(t.elem):
        inner = _go_redact_expr("(*in)", t.elem, depth + 1)
        ty = go_type(t)
        return f"func(in {ty}) {ty} {{ if in == nil {{ return nil }}; r := {inner}; return &r }}({expr})"
    return None


def _contains_message(t: Type) -> bool:
    return t.kind == "message" or (t.elem is not None and _contains_message(t.elem))


def emit_go(schema: Schema, pkg: str) -> str:
    g = _GoFile()
    o: list = []

    for e in schema.enums.values():
        o += comment_lines(e.doc or f"{e.name} 是 schema 里声明的枚举。", "")
        o += [f"type {e.name} string", "", "const ("]
        o += [f'\t{e.name}{pascal(v)} {e.name} = "{v}"' for v in e.values]
        o += [")", "", f"// Valid 报告 e 是否是 schema 里声明过的取值。", f"func (e {e.name}) Valid() bool {{",
              "\tswitch e {", f"\tcase {', '.join(e.name + pascal(v) for v in e.values)}:", "\t\treturn true", "\t}",
              "\treturn false", "}", ""]

    for m in schema.messages.values():
        n = m.name
        o += comment_lines(m.doc or f"{n} 是 schema 里声明的配置结构。", "")
        o.append(f"type {n} struct {{")
        for f in m.fields:
            o += comment_lines(f.doc, "\t")
            o.append(f'\t{pascal(f.key)} {go_type(f.type)} `yaml:"{f.key}"`')
        o += ["}", ""]

        # 默认值：Go 的零值分不清"没写"和"写了 0"，所以先构造带默认值的对象，再让 yaml 解码覆盖上去。
        o += [f"// New{n} 返回填好 schema 默认值的 {n}；加载时在它上面解码，没写的键保持默认值。",
              f"func New{n}() {n} {{", f"\tc := {n}{{}}"]
        for f in m.fields:
            if f.has_default:
                o.append(f"\tc.{pascal(f.key)} = {go_default(f)}")
            elif f.type.kind == "message":
                o.append(f"\tc.{pascal(f.key)} = New{f.type.name}()")
        o += ["\treturn c", "}", ""]

        # 必填：Go 的零值判断不了"有没有写"，由加载器按这份清单检查键是否存在。
        req = [f.key for f in m.fields if f.required]
        o += [f"// RequiredKeys 列出必填的键。Go 的零值分不清没写还是写了 0，请由加载器检查这些键是否存在。",
              f"func ({n}) RequiredKeys() []string {{",
              "\treturn " + ("[]string{" + ", ".join(go_str(k) for k in req) + "}" if req else "nil"), "}", ""]

        # Validate
        o += [f"// Validate 校验取值范围、枚举取值，并递归校验嵌套结构。", f"func (c {n}) Validate() error {{"]
        for f in m.fields:
            if f.min_count:
                g.uses_fmt = True
                o += [f"\tif len(c.{pascal(f.key)}) < {f.min_count} {{",
                      f'\t\treturn fmt.Errorf("%s: 至少需要 {f.min_count} 个元素", {go_str(f.key)})', "\t}"]
            _go_validate(o, g, f"c.{pascal(f.key)}", f.type, go_str(f.key), "\t", 0, f.range)
        o += ["\treturn nil", "}", ""]

        # Redacted
        o += [f"// Redacted 返回打码后的副本：secret 字段改成 \"***\"（字符串）或零值，嵌套结构递归处理，原对象不变。",
              f"func (c {n}) Redacted() {n} {{"]
        for f in m.fields:
            fld = f"c.{pascal(f.key)}"
            if f.secret:
                o.append(f'\t{fld} = "***"' if f.type.kind == "scalar" and f.type.name == "string"
                         else f"\t{fld} = {go_zero(f.type)}")
            else:
                r = _go_redact_expr(fld, f.type, 0)
                if r:
                    o.append(f"\t{fld} = {r}")
        o += ["\treturn c", "}", ""]

        # NonReloadableChanges
        o += [f"// NonReloadableChanges 列出 next 相对 c 里不允许热更却变了的字段（点号路径）。",
              f"func (c {n}) NonReloadableChanges(next {n}) []string {{", "\tvar out []string"]
        for f in m.fields:
            if f.reloadable:
                continue
            fld = pascal(f.key)
            if f.type.kind == "message":
                o += [f"\tfor _, p := range c.{fld}.NonReloadableChanges(next.{fld}) {{",
                      f'\t\tout = append(out, {go_str(f.key + ".")} + p)', "\t}"]
            else:
                g.uses_reflect = True
                o += [f"\tif !reflect.DeepEqual(c.{fld}, next.{fld}) {{", f"\t\tout = append(out, {go_str(f.key)})",
                      "\t}"]
        o += ["\treturn out", "}", ""]

    imports = [i for i, used in (("fmt", g.uses_fmt), ("reflect", g.uses_reflect)) if used]
    head = [f"// Code generated by tools/configgen from {schema.path.name}. DO NOT EDIT.", "", f"package {pkg}", ""]
    if imports:
        head += ["import ("] + [f'\t"{i}"' for i in imports] + [")", ""]
    return gofmt("\n".join(head + o).rstrip() + "\n")


def gofmt(src: str) -> str:
    exe = shutil.which("gofmt")
    if not exe:
        return src
    r = subprocess.run([exe], input=src, capture_output=True, text=True)
    return r.stdout if r.returncode == 0 else src


# ---------------------------------------------------------------------------------------------
# JSON Schema（给编辑器补全、拼写检查用）
# ---------------------------------------------------------------------------------------------


def _json_type(t: Type, rng=None) -> dict:
    k = t.kind
    if k == "scalar":
        jt = SCALARS[t.name][2]
        d: dict = {"type": jt}
        lo, hi = SCALARS[t.name][3], SCALARS[t.name][4]
        if rng:
            lo, hi = rng
        if lo is not None:
            d["minimum"] = lo
        if hi is not None:
            d["maximum"] = hi
        return d
    if k == "enum":
        return {"$ref": f"#/definitions/{t.name}"}
    if k == "message":
        return {"$ref": f"#/definitions/{t.name}"}
    inner = _json_type(t.elem)
    if k == "list":
        return {"type": "array", "items": inner}
    if k == "map":
        return {"type": "object", "additionalProperties": inner}
    return {"anyOf": [inner, {"type": "null"}]}  # optional


def _collect(t: Type, out: dict) -> None:
    if t.kind in ("enum", "message") and t.name not in out:
        out[t.name] = t.ref
        if t.kind == "message":
            for f in t.ref.fields:
                _collect(f.type, out)
    if t.elem:
        _collect(t.elem, out)


def emit_jsonschema(root: Message) -> str:
    refs: dict = {root.name: root}
    for f in root.fields:
        _collect(f.type, refs)
    defs = {}
    for name, ref in sorted(refs.items()):
        if isinstance(ref, Enum):
            defs[name] = {"enum": list(ref.values), **({"description": ref.doc.strip()} if ref.doc.strip() else {})}
            continue
        props, required = {}, []
        for f in ref.fields:
            p = _json_type(f.type, f.range)
            if f.doc.strip():
                p["description"] = f.doc.strip()
            if f.min_count:
                p["minItems" if f.type.kind == "list" else "minProperties"] = f.min_count
            if f.has_default:
                p["default"] = f.default
            if f.secret:
                p["writeOnly"] = True
            if f.env_value:
                p["x-env-value"] = True  # 自定义关键字：env/*.yaml 只能覆盖这些字段，且每个环境都要写（测试用）
            if "$ref" in p and len(p) > 1:
                p = {"allOf": [{"$ref": p.pop("$ref")}], **p}
            props[f.key] = p
            if f.required:
                required.append(f.key)
        d = {"type": "object", "additionalProperties": False, "properties": props}
        if required:
            d["required"] = required
        if ref.doc.strip():
            d["description"] = ref.doc.strip()
        defs[name] = d
    doc = {"$schema": "http://json-schema.org/draft-07/schema#", "title": root.name,
           "$ref": f"#/definitions/{root.name}", "definitions": defs}
    return json.dumps(doc, indent=2, ensure_ascii=False) + "\n"



# ---------------------------------------------------------------------------------------------
# 命令行
# ---------------------------------------------------------------------------------------------


def write_if_changed(path: Path, content: str) -> bool:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_text(encoding="utf-8") == content:
        return False
    path.write_text(content, encoding="utf-8", newline="\n")
    return True


def run(args: argparse.Namespace) -> list:
    schemas = load_schemas(args.schema_dir, args.include, args.protoc, args.schema)

    # Go 所有 schema 共用一个包，类型名必须全局唯一（C++ 按 package 分命名空间，没有这个限制）
    if args.go_out:
        seen: dict = {}
        for s in schemas:
            for n in list(s.enums) + list(s.messages):
                if n in seen and seen[n] != s.name:
                    raise SchemaError(f"Go 同一个包里类型名 '{n}' 重复：{seen[n]} 与 {s.name}")
                seen[n] = s.name

    written = []
    expected: set = set()
    for s in schemas:
        outputs = []
        if args.cpp_out:
            outputs.append((Path(args.cpp_out) / f"{s.name}_config.h", emit_cpp(s)))
        if args.go_out:
            outputs.append((Path(args.go_out) / f"{s.name}_config.go", emit_go(s, args.go_package)))
        if args.jsonschema_out:
            for m in s.messages.values():
                if m.root:
                    outputs.append((Path(args.jsonschema_out) / f"{snake(m.name)}.schema.json", emit_jsonschema(m)))
        for path, content in outputs:
            expected.add(path)
            if write_if_changed(path, content):
                written.append(path)
    if args.prune:
        prune_stale(args, expected)
    return written


# 只清理本生成器会产出的文件名模式，不碰输出目录里其它东西。
_GENERATED_PATTERNS = {"cpp_out": "*_config.h", "go_out": "*_config.go", "jsonschema_out": "*.schema.json"}


def prune_stale(args: argparse.Namespace, expected: set) -> None:
    """删掉输出目录里本次没有产出的旧文件（schema 被删 / 改名后留下的）。"""
    for attr, pattern in _GENERATED_PATTERNS.items():
        out = getattr(args, attr)
        if not out or not Path(out).is_dir():
            continue
        for f in sorted(Path(out).glob(pattern)):
            if f not in expected:
                f.unlink()
                print(f"configgen: 删除过期文件 {f}")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="配置 schema（proto）生成器")
    ap.add_argument("--schema-dir", required=True, help="schema（*.proto）所在目录")
    ap.add_argument("--schema", action="append", help="只处理这些 schema（文件名，不带 .proto）；默认目录下全部")
    ap.add_argument("-I", "--include", action="append", default=[],
                    help="额外的 proto 搜索目录（options.proto 不在 schema 目录时用），可重复")
    ap.add_argument("--protoc", help="protoc 的路径；默认取环境变量 PROTOC，再找 PATH 上的 protoc / protoc.exe")
    ap.add_argument("--cpp-out", help="C++ 头文件输出目录")
    ap.add_argument("--go-out", help="Go 源文件输出目录")
    ap.add_argument("--go-package", default="config", help="生成的 Go 包名，默认 config")
    ap.add_argument("--jsonschema-out", help="JSON Schema 输出目录（只为 root 的 message 生成）")
    ap.add_argument("--prune", action="store_true",
                    help="生成后删除输出目录里本次没产出的旧文件（不能与 --schema 同用）")
    ap.add_argument("--check", action="store_true", help="只校验 schema，不生成")
    ap.add_argument("-v", "--verbose", action="store_true", help="列出实际写入的文件")
    args = ap.parse_args(argv)
    if args.prune and args.schema:
        ap.error("--prune 要处理全部 schema，不能与 --schema 同用")
    if args.check:
        args.cpp_out = args.go_out = args.jsonschema_out = None
    elif not (args.cpp_out or args.go_out or args.jsonschema_out):
        ap.error("至少给一个 --cpp-out / --go-out / --jsonschema-out，或用 --check")
    try:
        written = run(args)
    except SchemaError as e:
        print(f"configgen: {e}", file=sys.stderr)
        return 1
    if args.verbose:
        for p in written:
            print(f"configgen: 写入 {p}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
