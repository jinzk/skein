#!/usr/bin/env python3
"""Standard library, FFI, encoding, traits and processes.

Extracted from the original test_skein.py main() body, lines 7539-8920.
"""
from harness import *  # noqa: F401,F403


def run_cases():
    ffi_probe = run("import std;\nkind_of(std.ffi)\n")
    has_ffi = "= dict" in ffi_probe.stdout
    if sys.platform.startswith("linux") and has_ffi:
        import ctypes.util
        libm = ctypes.util.find_library("m")
        libc = ctypes.util.find_library("c")
        if libm and libc:
            check(
                "std ffi basic calls",
                f'import std;\n'
                f'let m = std.ffi.open("{libm}");\n'
                f'std.ffi.call(m, "cos", "f64", ["f64"], [0.0])\n'
                f'let c = std.ffi.open("{libc}");\n'
                f'std.ffi.call(c, "strlen", "i64", ["cstring"], ["hello"])\n'
                f'let x = bytes_from_string("hello");\n'
                f'let y = bytes_from_string("hello");\n'
                f'std.ffi.call(c, "memcmp", "i32", '
                f'["bytes", "bytes", "usize"], [x, y, 5])\n'
                f'let buf = std.ffi.buffer(8);\n'
                f'std.ffi.call(c, "memcpy", "void", '
                f'["buffer", "bytes", "usize"], [buf, x, 5]);\n'
                f'bytes_to_string(std.ffi.buffer_bytes(buf, 5))\n'
                f'std.ffi.buffer_len(buf)\n'
                f'std.ffi.buffer_free(buf);\n'
                f'std.ffi.close(m);\nstd.ffi.close(c);\n',
                stdout_all=["= 1.0", "= 5", "= 0", "= hello", "= 8"],
            )

    if has_ffi and FFI_SHIM:
        shim = skein_string(FFI_SHIM)
        check(
            "std ffi scalar types",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            'std.ffi.call(lib, "shim_i8", "i8", ["i8"], [-5])\n'
            'std.ffi.call(lib, "shim_u8", "u8", ["u8"], [250])\n'
            'std.ffi.call(lib, "shim_i16", "i16", ["i16"], [-30000])\n'
            'std.ffi.call(lib, "shim_u16", "u16", ["u16"], [60000])\n'
            'std.ffi.call(lib, "shim_i32", "i32", ["i32"], [-100000])\n'
            'std.ffi.call(lib, "shim_u32", "u32", ["u32"], [4000000000])\n'
            'std.ffi.call(lib, "shim_i64", "i64", ["i64"], [-9000000000])\n'
            'std.ffi.call(lib, "shim_u64", "u64", ["u64"], [9000000000])\n'
            'std.ffi.call(lib, "shim_f32", "f32", ["f32"], [1.5])\n'
            'std.ffi.call(lib, "shim_f64", "f64", ["f64"], [2.5])\n'
            'std.ffi.call(lib, "shim_cstring", "cstring", '
            '["cstring"], ["hello"])\n'
            'std.ffi.call(lib, "shim_usize", "usize", ["usize"], [123])\n'
            'std.ffi.call(lib, "shim_isize", "isize", ["isize"], [-123])\n'
            "std.ffi.close(lib);\n",
            stdout_all=[
                "= -5", "= 250", "= -30000", "= 60000",
                "= -100000", "= 4000000000", "= -9000000000",
                "= 9000000000", "= 1.5", "= 2.5", "= hello",
                "= 123", "= -123",
            ],
        )
        check(
            "std ffi u64 overflow",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            'std.ffi.call(lib, "shim_u64_max", "u64", [], [])\n'
            "std.ffi.close(lib);\n",
            stderr="u64 return value exceeds int64 range",
        )
        if sys.maxsize > 2**31 - 1:
            check(
                "std ffi usize overflow",
                "import std;\n"
                f"let lib = std.ffi.open({shim});\n"
                'std.ffi.call(lib, "shim_usize_max", "usize", [], [])\n'
                "std.ffi.close(lib);\n",
                stderr="usize return value exceeds int64 range",
            )
        check(
            "std ffi library error",
            'import std;\nstd.ffi.open("/definitely/not/a/library.so")\n',
            stderr="cannot load",
        )
        check(
            "std ffi symbol error",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            'std.ffi.call(lib, "shim_missing_symbol", "void", [], [])\n'
            "std.ffi.close(lib);\n",
            stderr="symbol not found",
        )
        check(
            "std ffi call after close",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            "std.ffi.close(lib);\n"
            'std.ffi.call(lib, "shim_i8", "i8", ["i8"], [1])\n',
            stderr="ffi library is closed",
        )
        check(
            "std ffi double close",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            "std.ffi.close(lib);\n"
            "std.ffi.close(lib);\n",
            stderr="ffi library is closed",
        )
        check(
            "std ffi direct bytes return",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            'std.ffi.call(lib, "shim_cstring", "bytes", '
            '["cstring"], ["hello"])\n'
            "std.ffi.close(lib);\n",
            stderr="cannot be returned directly",
        )
        check(
            "std ffi buffer output",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            "let buf = std.ffi.buffer(8);\n"
            'std.ffi.call(lib, "shim_buffer_write", "i32", '
            '["buffer", "usize"], [buf, 8])\n'
            "bytes_to_string(std.ffi.buffer_bytes(buf, 5))\n"
            "std.ffi.buffer_len(buf)\n"
            "std.ffi.buffer_free(buf);\n"
            "std.ffi.close(lib);\n",
            stdout_all=["= 5", "= hello", "= 8"],
        )
        check(
            "std ffi buffer bounds",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            "let buf = std.ffi.buffer(4);\n"
            "std.ffi.buffer_bytes(buf, 5)\n"
            "std.ffi.buffer_free(buf);\n"
            "std.ffi.buffer_len(buf)\n"
            "std.ffi.close(lib);\n",
            stderr="length out of range",
        )
        check(
            "std ffi freed buffer",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            "let buf = std.ffi.buffer(4);\n"
            "std.ffi.buffer_free(buf);\n"
            "std.ffi.buffer_len(buf)\n"
            "std.ffi.close(lib);\n",
            stderr="ffi buffer is freed",
        )
        point_decl = (
            'let Point = std.ffi.struct("Point", '
            '[["x", "f64"], ["y", "f64"]]);\n'
        )
        check(
            "std ffi struct fields",
            "import std;\n"
            + point_decl
            + 'let p = std.ffi.new(Point, {"x": 1.5, "y": 2.5});\n'
            "p.x\n"
            "p.y\n"
            "p.x = 4.0;\n"
            "p.x\n"
            "std.ffi.to_dict(p)\n"
            "std.ffi.sizeof(Point)\n"
            "std.ffi.alignof(Point)\n"
            'std.ffi.offsetof(Point, "y")\n'
            'std.ffi.offsetof(p, "y")\n',
            stdout_all=[
                "= 1.5", "= 2.5", "= 4.0", "= {x: 4.0, y: 2.5}",
                "= 16", "= 8", "= 8", "= 8",
            ],
        )
        check(
            "std ffi address_of",
            "import std;\n"
            + point_decl
            + f"let lib = std.ffi.open({shim});\n"
            'let p = std.ffi.new(Point, {"x": 3.0, "y": 4.0});\n'
            "let ptr = std.ffi.address_of(p);\n"
            'std.ffi.call(lib, "shim_point_dot", "f64", '
            "[std.ffi.pointer(Point)], [ptr])\n"
            "std.ffi.close(lib);\n",
            stdout="= 25.0",
        )
        check(
            "std ffi struct by value",
            "import std;\n"
            + point_decl
            + f'let lib = std.ffi.open({shim});\n'
            'let p = std.ffi.new(Point, {"x": 1.5, "y": 2.5});\n'
            'std.ffi.call(lib, "shim_point_sum", "f64", [Point], [p])\n'
            'let made = std.ffi.call(lib, "shim_point_make", Point, '
            '["f64", "f64"], [1.0, 2.0]);\n'
            "made.x\n"
            "made.y\n"
            "std.ffi.close(lib);\n",
            stdout_all=["= 4.0", "= 1.0", "= 2.0"],
        )
        check(
            "std ffi struct padding",
            "import std;\n"
            'let Pair = std.ffi.struct("Pair", '
            '[["tag", "i8"], ["value", "i64"]]);\n'
            "std.ffi.sizeof(Pair)\n"
            f'let lib = std.ffi.open({shim});\n'
            'let pair = std.ffi.new(Pair, {"tag": -3, "value": 9000000000});\n'
            'std.ffi.call(lib, "shim_pair_sum", "i64", [Pair], [pair])\n'
            'let made = std.ffi.call(lib, "shim_pair_make", Pair, '
            '["i8", "i64"], [-1, 41]);\n'
            "made.tag\n"
            "made.value\n"
            "std.ffi.close(lib);\n",
            stdout_all=["= 16", "= 8999999997", "= -1", "= 41"],
        )
        check(
            "std ffi pointer arguments",
            "import std;\n"
            + point_decl
            + f'let lib = std.ffi.open({shim});\n'
            "let raw = std.ffi.alloc(Point);\n"
            "let view = std.ffi.deref(raw);\n"
            "view.x = 3.0;\n"
            "view.y = 4.0;\n"
            'std.ffi.call(lib, "shim_point_dot", "f64", ["ptr"], [raw])\n'
            'std.ffi.call(lib, "shim_point_scale", "void", '
            '["ptr", "f64"], [raw, 2.0]);\n'
            "std.ffi.deref(raw).x\n"
            "std.ffi.deref(raw).y\n"
            "std.ffi.close(lib);\n",
            stdout_all=["= 25.0", "= 6.0", "= 8.0"],
        )
        check(
            "std ffi pointer return",
            "import std;\n"
            + point_decl
            + f'let lib = std.ffi.open({shim});\n'
            'let global = std.ffi.call(lib, "shim_point_global", '
            "std.ffi.pointer(Point), [], []);\n"
            "std.ffi.deref(global).x\n"
            "std.ffi.deref(global).y\n"
            "std.ffi.close(lib);\n",
            stdout_all=["= 3.0", "= 4.0"],
        )
        check(
            "std ffi nested struct and array",
            "import std;\n"
            + point_decl
            + 'let Nested = std.ffi.struct("Nested", '
            '[["point", Point], ["tag", "i32"]]);\n'
            'let Words = std.ffi.struct("Words", '
            '[["values", std.ffi.array("i32", 3)]]);\n'
            f'let lib = std.ffi.open({shim});\n'
            'let n = std.ffi.new(Nested, '
            '{"point": {"x": 1.0, "y": 2.0}, "tag": 3});\n'
            "n.point.x\n"
            "n.point.x = 10.0;\n"
            "n.point.x\n"
            'std.ffi.call(lib, "shim_nested_score", "f64", '
            '[Nested], [n]);\n'
            'let w = std.ffi.new(Words, {"values": [1, 2, 3]});\n'
            "std.ffi.get(w.values, 1)\n"
            "std.ffi.set(w.values, 1, 9);\n"
            'std.ffi.call(lib, "shim_words_sum", "i32", [Words], [w])\n'
            "std.ffi.close(lib);\n",
            stdout_all=["= 1.0", "= 10.0", "= 15.0", "= 2", "= 9",
                        "= 13"],
        )
        check(
            "std ffi struct errors",
            "import std;\n" + point_decl + 'let p = std.ffi.new(Point);\n'
            "p.z\n",
            stderr="has no field z",
        )
        check(
            "std ffi null deref",
            "import std;\n" + point_decl
            + "std.ffi.deref(std.ffi.ptr(Point))\n",
            stderr="null pointer",
        )
        check(
            "std ffi field type error",
            "import std;\n" + point_decl
            + 'let p = std.ffi.new(Point);\np.x = "bad";\n',
            stderr="expected a number",
        )
        check(
            "std ffi new scalar rejected",
            'import std;\nstd.ffi.new("i32")\n',
            stderr="expects a struct or array type",
        )
        check(
            "std ffi empty struct rejected",
            'import std;\nstd.ffi.struct("Empty", [])\n',
            stderr="must have at least one field",
        )
        check(
            "std ffi duplicate field rejected",
            "import std;\n"
            'std.ffi.struct("P", [["x", "i32"], ["x", "i32"]])\n',
            stderr="duplicate field x",
        )
        check(
            "std ffi void field rejected",
            "import std;\n"
            'std.ffi.struct("P", [["x", "void"]])\n',
            stderr="cannot be passed by value",
        )
        check(
            "std ffi array bounds",
            "import std;\n"
            'let A = std.ffi.array("i32", 3);\n'
            "let a = std.ffi.new(A, [1, 2, 3]);\n"
            "std.ffi.get(a, 5)\n",
            stderr="out of range",
        )
        check(
            "std ffi struct dict output",
            "import std;\n"
            + point_decl
            + 'let p = std.ffi.new(Point, {"x": 1, "y": 2});\n'
            'std.ffi.to_dict(p)["y"]\n',
            stdout="= 2.0",
        )
        check(
            "std ffi nested cstring keepalive",
            "import std;\n"
            'let Inner = std.ffi.struct("Inner", '
            '[["text", "cstring"], ["n", "i32"]]);\n'
            'let Outer = std.ffi.struct("Outer", [["inner", Inner]]);\n'
            "let o = std.ffi.new(Outer);\n"
            'o.inner.text = "hello";\n'
            "o.inner.text\n"
            "o.inner.n = 5;\n"
            "o.inner.n\n"
            "o.inner.text\n"
            "let p = std.ffi.alloc(Inner);\n"
            'std.ffi.set(p, 0, {"text": "world", "n": 9});\n'
            "std.ffi.get(p, 0).text\n"
            "std.ffi.get(p, 0).n\n",
            stdout_all=["= hello", "= 5", "= hello", "= world", "= 9"],
        )
        check(
            "std ffi pointer to array indexing",
            "import std;\n"
            'let A = std.ffi.array("i32", 3);\n'
            "let a = std.ffi.alloc(A);\n"
            "std.ffi.set(a, 0, 7);\n"
            "std.ffi.set(a, 2, 9);\n"
            "std.ffi.get(a, 0)\n"
            "std.ffi.get(a, 2)\n"
            "std.ffi.get(a, 3)\n",
            stdout_all=["= 7", "= 9"],
            stderr="out of range",
        )
        check(
            "std ffi call address",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            'let fn = std.ffi.call(lib, "shim_get_cmp", "ptr", [], []);\n'
            'let Cmp = std.ffi.signature("i32", ["i32", "i32"]);\n'
            "std.ffi.call_address(fn, Cmp, [9, 4])\n"
            'std.ffi.call_address(fn, "i32", ["i32", "i32"], [9, 4])\n'
            "std.ffi.close(lib);\n",
            stdout_all=["= 5", "= 5"],
        )
        check(
            "std ffi errno",
            "import std;\n"
            "func probe() {\n"
            "  std.ffi.set_errno(42);\n"
            "  return std.ffi.errno();\n"
            "}\n"
            "probe()\n",
            stdout="= 42",
        )
        check(
            "std ffi symbol",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            'let sym = std.ffi.symbol(lib, "shim_i32");\n'
            'std.ffi.call_address(sym, "i32", ["i32"], [42])\n'
            "std.ffi.close(lib);\n",
            stdout="= 42",
        )
        check(
            "std ffi call variadic",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            'std.ffi.call_variadic(lib, "shim_sum_varargs", "i32", '
            '["i32", "i32", "i32"], [2, 5, 7], 1)\n'
            "std.ffi.close(lib);\n",
            stdout="= 12",
        )
        check(
            "std ffi manifest variadic",
            "import std;\n"
            f"let path = {shim};\n"
            'let spec = {"ffi": 1, "functions": {"shim_sum_varargs": '
            '{"result": "i32", "args": ["i32"], "variadic": true}}};\n'
            "let lib = std.ffi.open(path, spec);\n"
            'std.ffi.call(lib, "shim_sum_varargs", [0])\n'
            'std.ffi.call_variadic(lib, "shim_sum_varargs", [2], '
            '["i32", "i32"], [5, 7])\n'
            "std.ffi.close(lib);\n",
            stdout_all=["= 0", "= 12"],
        )
        check(
            "std ffi manifest variadic needs typed call",
            "import std;\n"
            f"let path = {shim};\n"
            'let spec = {"ffi": 1, "functions": {"shim_sum_varargs": '
            '{"result": "i32", "args": ["i32"], "variadic": true}}};\n'
            "let lib = std.ffi.open(path, spec);\n"
            'std.ffi.call(lib, "shim_sum_varargs", [2, 5, 7])\n',
            stderr="call_variadic",
        )
        check(
            "std ffi calling convention",
            "import std;\n"
            f"let path = {shim};\n"
            'let spec = {"ffi": 1, "functions": {"shim_i32": '
            '{"result": "i32", "args": ["i32"], '
            '"calling_convention": "stdcall"}}};\n'
            "let lib = std.ffi.open(path, spec);\n"
            'std.ffi.call(lib, "shim_i32", [42])\n'
            "std.ffi.close(lib);\n",
            stdout="= 42",
        )
        check(
            "std ffi rejects unknown calling convention",
            "import std;\n"
            'std.ffi.manifest({"ffi": 1, "functions": {"f": '
            '{"result": "i32", "calling_convention": "vectorcall"}}})\n',
            stderr="unsupported calling convention",
        )
        check(
            "std ffi callback field call",
            "import std;\n"
            f"let path = {shim};\n"
            'let spec = {"ffi": 1, "types": {"Hooks": {"kind": "struct", '
            '"fields": {"cmp": {"kind": "callback", "result": "i32", '
            '"args": ["i32", "i32"]}}}}, "functions": {'
            '"shim_make_hooks": {"result": "Hooks"}, '
            '"shim_call_hook": {"result": "i32", '
            '"args": ["Hooks", "i32", "i32"]}}};\n'
            "let lib = std.ffi.open(path, spec);\n"
            'let hooks = std.ffi.call(lib, "shim_make_hooks", []);\n'
            'let Cmp = std.ffi.signature("i32", ["i32", "i32"]);\n'
            "std.ffi.call_address(hooks.cmp, Cmp, [9, 4])\n"
            'std.ffi.call(lib, "shim_call_hook", [hooks, 9, 4])\n'
            "std.ffi.close(lib);\n",
            stdout_all=["= 5", "= 5"],
        )
        check(
            "std ffi callback basic",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            'let Cmp = std.ffi.signature("i32", ["i32", "i32"]);\n'
            "let calls = 0;\n"
            'let cb = std.ffi.callback(Cmp, func(a, b) { calls = calls + 1; '
            "if (a < b) { return -1; } if (a > b) { return 1; } "
            "return 0; });\n"
            'std.ffi.call(lib, "shim_apply_cmp", "i32", '
            '[Cmp, "i32", "i32"], [cb, 3, 5])\n'
            'std.ffi.call(lib, "shim_apply_cmp", "i32", '
            '[Cmp, "i32", "i32"], [cb, 7, 5])\n'
            'std.ffi.call(lib, "shim_apply_cmp_twice", "i32", '
            '[Cmp, "i32", "i32"], [cb, 3, 7])\n'
            "calls\n"
            "std.ffi.close(lib);\n",
            stdout_all=["= -1", "= 1", "= -2", "= 4"],
        )
        check(
            "std ffi callback struct and pointer args",
            "import std;\n"
            + point_decl
            + f'let lib = std.ffi.open({shim});\n'
            'let Score = std.ffi.signature("f64", [Point]);\n'
            'let score = std.ffi.callback(Score, func(p) { '
            "return p.x * 2.0 + p.y; });\n"
            'let p = std.ffi.new(Point, {"x": 1.5, "y": 2.0});\n'
            'std.ffi.call(lib, "shim_apply_point", "f64", '
            "[Score, Point], [score, p])\n"
            'let PCmp = std.ffi.signature("i32", ["ptr", "ptr"]);\n'
            "let pcmp = std.ffi.callback(PCmp, func(a, b) {\n"
            "  let x = std.ffi.deref(std.ffi.cast(a, "
            'std.ffi.pointer("i32")));\n'
            "  let y = std.ffi.deref(std.ffi.cast(b, "
            'std.ffi.pointer("i32")));\n'
            "  return x - y;\n"
            "});\n"
            'std.ffi.call(lib, "shim_apply_ptr_cmp", "i32", '
            '[PCmp, "i32", "i32"], [pcmp, 3, 5])\n'
            "std.ffi.close(lib);\n",
            stdout_all=["= 5.0", "= -2"],
        )
        check(
            "std ffi callback string return",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            'let S = std.ffi.signature("cstring", ["cstring"]);\n'
            'let cb = std.ffi.callback(S, func(text) { '
            'return text + "!"; });\n'
            'std.ffi.call(lib, "shim_apply_string", "cstring", '
            '[S, "cstring"], [cb, "hi"])\n'
            "std.ffi.close(lib);\n",
            stdout="= hi!",
        )
        check(
            "std ffi callback error propagates",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            'let Cmp = std.ffi.signature("i32", ["i32", "i32"]);\n'
            "let cb = std.ffi.callback(Cmp, func(a, b) { return 1 / 0; });\n"
            'std.ffi.call(lib, "shim_apply_cmp", "i32", '
            '[Cmp, "i32", "i32"], [cb, 3, 5])\n',
            stderr="division by zero",
        )
        check(
            "std ffi callback result type",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            'let Cmp = std.ffi.signature("i32", ["i32", "i32"]);\n'
            'let cb = std.ffi.callback(Cmp, func(a, b) { return "x"; });\n'
            'std.ffi.call(lib, "shim_apply_cmp", "i32", '
            '[Cmp, "i32", "i32"], [cb, 3, 5])\n',
            stderr="ffi callback result",
        )
        check(
            "std ffi callback rejects async",
            "import std;\n"
            'let Cmp = std.ffi.signature("i32", ["i32", "i32"]);\n'
            "std.ffi.callback(Cmp, async func(a, b) { return 0; })\n",
            stderr="ffi callbacks must be synchronous functions",
        )
        check(
            "std ffi callback rejects generator",
            "import std;\n"
            'let Cmp = std.ffi.signature("i32", ["i32", "i32"]);\n'
            "std.ffi.callback(Cmp, func(a, b) { yield 0; })\n",
            stderr="ffi callbacks must be synchronous functions",
        )
        manifest_spec = (
            "let spec = {\n"
            '  "ffi": 1,\n'
            '  "types": {\n'
            '    "Point": {"kind": "struct", '
            '"fields": {"x": "f64", "y": "f64"}},\n'
            '    "Cmp": {"kind": "callback", "result": "i32", '
            '"args": ["i32", "i32"]},\n'
            '    "PointPtr": {"kind": "pointer", "to": "Point"}\n'
            "  },\n"
            '  "functions": {\n'
            '    "shim_point_sum": {"result": "f64", "args": ["Point"]},\n'
            '    "shim_point_make": {"result": "Point", '
            '"args": ["f64", "f64"]},\n'
            '    "shim_apply_cmp": {"result": "i32", '
            '"args": ["Cmp", "i32", "i32"]},\n'
            '    "shim_alloc_point": {"result": "PointPtr", '
            '"returns_owned": {"free": "shim_free_point"}},\n'
            '    "shim_free_count": {"result": "i32"},\n'
            '    "shim_consume_point": {"result": "void", '
            '"args": ["PointPtr"], "consumes": [0]},\n'
            '    "shim_consume_free_point": {"result": "void", '
            '"args": ["PointPtr"], "consumes": [0]},\n'
            '    "shim_save_cmp": {"result": "void", "args": ["Cmp"], '
            '"retains": [0]},\n'
            '    "shim_call_saved": {"result": "i32", '
            '"args": ["i32", "i32"]},\n'
            '    "shim_save_text": {"result": "void", "args": ["cstring"], '
            '"retains": [0]},\n'
            '    "shim_saved_text_value": {"result": "cstring"},\n'
            '    "shim_churn_text": {"result": "void", "args": ["usize"]}\n'
            "  },\n"
            '  "constants": {"SHIM_MAX": 16}\n'
            "};\n"
        )
        check(
            "std ffi dynamic manifest",
            "import std;\n"
            f"let path = {shim};\n"
            + manifest_spec
            + "let lib = std.ffi.open(path, spec);\n"
            'std.ffi.call(lib, "shim_point_sum", '
            '[{"x": 1.5, "y": 2.5}])\n'
            'std.ffi.call(lib, "shim_point_make", [3.0, 4.0]).y\n'
            'std.ffi.constant(lib, "SHIM_MAX")\n'
            'let Point = std.ffi.type(lib, "Point");\n'
            "std.ffi.sizeof(Point)\n"
            "std.ffi.alignof(Point)\n"
            'let p = std.ffi.new(Point, {"x": 1.0, "y": 2.0});\n'
            'std.ffi.call(lib, "shim_point_sum", [p])\n'
            "std.ffi.close(lib);\n",
            stdout_all=["= 4.0", "= 4.0", "= 16", "= 16", "= 8", "= 3.0"],
        )
        check(
            "std ffi bound functions",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            'std.ffi.declare(lib, {"ffi": 1, "types": {"P": '
            '{"kind": "struct", "fields": {"x": "f64", "y": "f64"}}}, '
            '"functions": {"shim_point_sum": {"result": "f64", '
            '"args": ["P"]}, "shim_i32": {"result": "i32", '
            '"args": ["i32"]}}, "constants": {"K": 5}});\n'
            'let sum = std.ffi.function(lib, "shim_point_sum");\n'
            'sum({"x": 1.0, "y": 2.0})\n'
            "kind_of(sum)\n"
            'lib.shim_point_sum({"x": 3.0, "y": 4.0})\n'
            "lib.K\n"
            "std.ffi.close(lib);\n",
            stdout_all=["= 3.0", "= native", "= 7.0", "= 5"],
        )
        check(
            "std ffi declare incrementally",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            'std.ffi.declare(lib, {"ffi": 1, "types": {"P": '
            '{"kind": "struct", "fields": {"x": "f64", "y": "f64"}}}});\n'
            'std.ffi.declare(lib, {"ffi": 1, "functions": '
            '{"shim_point_sum": {"result": "f64", "args": ["P"]}}});\n'
            'std.ffi.call(lib, "shim_point_sum", '
            '[{"x": 1.0, "y": 2.0}])\n'
            'lib.shim_point_sum({"x": 2.0, "y": 2.0})\n'
            "std.ffi.close(lib);\n",
            stdout_all=["= 3.0", "= 4.0"],
        )
        check(
            "std ffi declare duplicate",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            'std.ffi.declare(lib, {"ffi": 1, "types": {"P": '
            '{"kind": "struct", "fields": {"x": "f64"}}}});\n'
            'std.ffi.declare(lib, {"ffi": 1, "types": {"P": '
            '{"kind": "struct", "fields": {"y": "f64"}}}});\n',
            stderr="duplicate manifest type P",
        )
        check(
            "std ffi bound function after close",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            'std.ffi.declare(lib, {"ffi": 1, "functions": '
            '{"shim_i32": {"result": "i32", "args": ["i32"]}}});\n'
            'let f = std.ffi.function(lib, "shim_i32");\n'
            "std.ffi.close(lib);\n"
            "f(1)\n",
            stderr="ffi library is closed",
        )
        check(
            "std ffi manifest type objects",
            "import std;\n"
            'let Point = std.ffi.struct("Point", {"x": "f64", "y": "f64"});\n'
            f"let lib = std.ffi.open({shim}, "
            '{"ffi": 1, "types": {"P": Point}, '
            '"functions": {"shim_point_sum": '
            '{"result": "f64", "args": ["P"]}}});\n'
            'std.ffi.call(lib, "shim_point_sum", '
            '[{"x": 1.0, "y": 2.0}])\n',
            stdout="= 3.0",
        )
        import json
        manifest_path = os.path.join(tempfile.gettempdir(),
                                     "skein_ffi_manifest_test.json")
        with open(manifest_path, "w", encoding="utf-8") as handle:
            json.dump({
                "ffi": 1,
                "abi": {
                    "pointer_bits": 64 if sys.maxsize > 2**32 else 32,
                    "endian": "native",
                    "calling_convention": "c",
                },
                "types": {
                    "Point": {
                        "kind": "struct",
                        "fields": {"x": "f64", "y": "f64"},
                    }
                },
                "functions": {
                    "shim_point_sum": {"result": "f64", "args": ["Point"]},
                },
            }, handle)
        check(
            "std ffi json manifest",
            "import std;\n"
            f"let lib = std.ffi.open({shim}, "
            f"{skein_string(manifest_path)});\n"
            'std.ffi.call(lib, "shim_point_sum", [{"x": 2.0, "y": 3.0}])\n'
            "std.ffi.close(lib);\n",
            stdout="= 5.0",
        )
        check(
            "std ffi manifest variables",
            "import std;\n"
            f"let path = {shim};\n"
            'let spec = {"ffi": 1, "variables": '
            '{"shim_global": {"type": "i32"}}};\n'
            "let lib = std.ffi.open(path, spec);\n"
            'std.ffi.variable(lib, "shim_global")\n'
            "lib.shim_global\n"
            "lib.shim_global = 11;\n"
            "lib.shim_global\n"
            'std.ffi.set_variable(lib, "shim_global", 13);\n'
            'std.ffi.variable(lib, "shim_global")\n'
            "std.ffi.close(lib);\n",
            stdout_all=["= 7", "= 7", "= 11", "= 13"],
        )
        check(
            "std ffi read-only variable",
            "import std;\n"
            f"let path = {shim};\n"
            'let spec = {"ffi": 1, "variables": '
            '{"shim_const_global": {"type": "i32", "const": true}}};\n'
            "let lib = std.ffi.open(path, spec);\n"
            'std.ffi.variable(lib, "shim_const_global")\n'
            "lib.shim_const_global = 1;\n",
            stdout="= 42",
            stderr="is read-only",
        )
        check(
            "std ffi manifest callbacks and ownership",
            "import std;\n"
            f"let path = {shim};\n"
            + manifest_spec
            + "let lib = std.ffi.open(path, spec);\n"
            'let Cmp = std.ffi.type(lib, "Cmp");\n'
            'let point = std.ffi.type(lib, "Point");\n'
            "let cmp = std.ffi.callback(Cmp, func(a, b) { return a - b; });\n"
            'std.ffi.call(lib, "shim_apply_cmp", [cmp, 9, 4])\n'
            'let owned = std.ffi.call(lib, "shim_alloc_point", []);\n'
            "std.ffi.deref(owned).x\n"
            "owned = null;\n"
            'std.ffi.call(lib, "shim_free_count", [])\n'
            "let saved = std.ffi.callback(Cmp, func(a, b) { return a + b; });\n"
            'std.ffi.call(lib, "shim_save_cmp", [saved]);\n'
            "saved = null;\n"
            'std.ffi.call(lib, "shim_call_saved", [3, 4])\n'
            'let raw = std.ffi.alloc(point);\n'
            'std.ffi.call(lib, "shim_consume_point", [raw]);\n'
            "std.ffi.close(lib);\n",
            stdout_all=["= 5", "= 1.0", "= 1", "= 7"],
        )
        check(
            "std ffi manifest consumes",
            "import std;\n"
            f"let path = {shim};\n"
            + manifest_spec
            + "let lib = std.ffi.open(path, spec);\n"
            'let Point = std.ffi.type(lib, "Point");\n'
            "let raw = std.ffi.alloc(Point);\n"
            'std.ffi.call(lib, "shim_consume_point", [raw]);\n'
            "std.ffi.deref(raw)\n",
            stderr="consumed by C",
        )
        check(
            "std ffi consumes transfers allocation",
            "import std;\n"
            f"let path = {shim};\n"
            + manifest_spec
            + "let lib = std.ffi.open(path, spec);\n"
            'let Point = std.ffi.type(lib, "Point");\n'
            "let raw = std.ffi.alloc(Point);\n"
            'std.ffi.call(lib, "shim_consume_free_point", [raw]);\n'
            "raw = null;\n"
            'std.ffi.call(lib, "shim_free_count", [])\n'
            "std.ffi.close(lib);\n",
            stdout="= 1",
        )
        check(
            "std ffi consumes owned pointer",
            "import std;\n"
            f"let path = {shim};\n"
            + manifest_spec
            + "let lib = std.ffi.open(path, spec);\n"
            'let owned = std.ffi.call(lib, "shim_alloc_point", []);\n'
            'std.ffi.call(lib, "shim_consume_free_point", [owned]);\n'
            "owned = null;\n"
            'std.ffi.call(lib, "shim_free_count", [])\n'
            "std.ffi.close(lib);\n",
            stdout="= 1",
        )
        check(
            "std ffi returns_owned int free",
            "import std;\n"
            f"let path = {shim};\n"
            'let spec = {"ffi": 1, "functions": {'
            '"shim_alloc_point": {"result": "ptr", "returns_owned": '
            '{"free": "shim_free_point_int", "free_result": "i32"}}, '
            '"shim_free_count": {"result": "i32"}}};\n'
            "let lib = std.ffi.open(path, spec);\n"
            'let owned = std.ffi.call(lib, "shim_alloc_point", []);\n'
            "owned = null;\n"
            'std.ffi.call(lib, "shim_free_count", [])\n'
            "std.ffi.close(lib);\n",
            stdout="= 1",
        )
        check(
            "std ffi rejects invalid free signature",
            "import std;\n"
            'std.ffi.manifest({"ffi": 1, "functions": {"f": '
            '{"result": "ptr", "returns_owned": {"free": "x", '
            '"free_args": ["i32"]}}}})\n',
            stderr="free must take one pointer argument",
        )
        check(
            "std ffi retains cstring",
            "import std;\n"
            f"let path = {shim};\n"
            + manifest_spec
            + "let lib = std.ffi.open(path, spec);\n"
            'let text = std.string.repeat("retained-", 64);\n'
            'std.ffi.call(lib, "shim_save_text", [text]);\n'
            'std.ffi.call(lib, "shim_churn_text", '
            "[std.string.len(text)]);\n"
            'std.ffi.call(lib, "shim_saved_text_value", [])\n'
            "std.ffi.close(lib);\n",
            stdout="= " + "retained-" * 64,
        )
        check(
            "std ffi library without manifest",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            'std.ffi.call(lib, "shim_i32", [1])\n',
            stderr="library has no manifest",
        )
        check(
            "std ffi manifest rejects bad version",
            'import std;\nstd.ffi.manifest({"ffi": 2})\n',
            stderr="unsupported manifest version",
        )
        check(
            "std ffi manifest rejects unknown type",
            "import std;\n"
            'std.ffi.manifest({"ffi": 1, "types": {"X": '
            '{"kind": "struct", "fields": {"a": "Missing"}}}})\n',
            stderr="unknown manifest type Missing",
        )
        check(
            "std ffi manifest rejects packed",
            "import std;\n"
            'std.ffi.manifest({"ffi": 1, "types": {"X": '
            '{"kind": "struct", "packed": true, '
            '"fields": {"a": "i32"}}}})\n',
            stderr="packed structs are not supported",
        )
        check(
            "std ffi manifest rejects consumes non-pointer",
            "import std;\n"
            'std.ffi.manifest({"ffi": 1, "functions": {"f": '
            '{"args": ["i32"], "consumes": [0]}}})\n',
            stderr="consumes a non-pointer argument",
        )
        check(
            "std ffi manifest rejects retains non-pointer",
            "import std;\n"
            'std.ffi.manifest({"ffi": 1, "functions": {"f": '
            '{"args": ["i32"], "retains": [0]}}})\n',
            stderr="retains a non-pointer argument",
        )
        check(
            "std ffi manifest rejects owned non-pointer",
            "import std;\n"
            'std.ffi.manifest({"ffi": 1, "functions": {"f": '
            '{"result": "i32", "returns_owned": true}}})\n',
            stderr="returns_owned needs a pointer result",
        )
        check(
            "std ffi manifest rejects bitfield",
            "import std;\n"
            'std.ffi.manifest({"ffi": 1, "types": {"X": '
            '{"kind": "struct", "fields": ['
            '{"name": "a", "type": "i32", "bits": 1}]}}})\n',
            stderr="bitfields are not supported",
        )
        check(
            "std ffi manifest rejects recursion",
            "import std;\n"
            'std.ffi.manifest({"ffi": 1, "types": {'
            '"A": {"kind": "struct", "fields": {"b": "B"}}, '
            '"B": {"kind": "struct", "fields": {"a": "A"}}}})\n',
            stderr="recursive by-value type",
        )

        check(
            "std ffi cross-thread callback",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            "let seen = 0;\n"
            'let Notify = std.ffi.signature("void", ["i32"]);\n'
            "let cb = std.ffi.callback(Notify, func(x) { seen = seen + x; });\n"
            'std.ffi.call(lib, "shim_spawn_notify", "void", '
            '[Notify, "i32"], [cb, 7]);\n'
            "std.task.wait(std.task.sleep(5));\n"
            "seen\n"
            "std.ffi.close(lib);\n",
            stdout="= 7",
        )
        check(
            "std ffi cross-thread callback must return void",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            'let Cmp = std.ffi.signature("i32", ["i32", "i32"]);\n'
            "let cb = std.ffi.callback(Cmp, func(a, b) { return a - b; });\n"
            'std.ffi.call(lib, "shim_spawn_cmp", "void", '
            '[Cmp, "i32", "i32"], [cb, 1, 2]);\n'
            "std.task.wait(std.task.sleep(5));\n",
            stderr="cross-thread callbacks must return void",
        )
        check(
            "std ffi call_async",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            'std.ffi.call_async(lib, "shim_i32", "i32", ["i32"], [21])\n'
            "std.ffi.close(lib);\n",
            stdout="= 21",
        )
        check(
            "std ffi call_async blocking callback",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            'let Cmp = std.ffi.signature("i32", ["i32", "i32"]);\n'
            "let cb = std.ffi.callback(Cmp, func(a, b) { return a - b; });\n"
            'std.ffi.call_async(lib, "shim_apply_cmp", "i32", '
            '[Cmp, "i32", "i32"], [cb, 9, 4])\n'
            "std.ffi.close(lib);\n",
            stdout="= 5",
        )
        check(
            "std ffi call_async callback error",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            'let Cmp = std.ffi.signature("i32", ["i32", "i32"]);\n'
            "let cb = std.ffi.callback(Cmp, func(a, b) { return 1 / 0; });\n"
            'std.ffi.call_async(lib, "shim_apply_cmp", "i32", '
            '[Cmp, "i32", "i32"], [cb, 1, 2])\n',
            stderr="division by zero",
        )
        check(
            "std ffi call_async nested ffi.call",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            'let Cmp = std.ffi.signature("i32", ["i32", "i32"]);\n'
            "let inner = std.ffi.callback(Cmp, func(a, b) { return a - b; });\n"
            "let outer = std.ffi.callback(Cmp, func(a, b) {\n"
            '  return std.ffi.call(lib, "shim_apply_cmp", "i32", '
            '[Cmp, "i32", "i32"], [inner, a, b]);\n'
            "});\n"
            'std.ffi.call_async(lib, "shim_apply_cmp", "i32", '
            '[Cmp, "i32", "i32"], [outer, 9, 4])\n',
            stdout="= 5",
        )
        check(
            "std ffi retain callback",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            'let Cmp = std.ffi.signature("i32", ["i32", "i32"]);\n'
            "let cb = std.ffi.callback(Cmp, func(a, b) { return a + b; });\n"
            'std.ffi.call(lib, "shim_save_cmp", "void", [Cmp], [cb]);\n'
            "std.ffi.retain(lib, cb);\n"
            "cb = null;\n"
            'std.ffi.call(lib, "shim_call_saved", "i32", '
            '["i32", "i32"], [3, 4])\n'
            "std.ffi.close(lib);\n",
            stdout="= 7",
        )
        check(
            "std ffi callback reentrant call",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            'let Cmp = std.ffi.signature("i32", ["i32", "i32"]);\n'
            "let inner = std.ffi.callback(Cmp, func(a, b) { "
            "return a - b; });\n"
            "let outer = std.ffi.callback(Cmp, func(a, b) {\n"
            '  return std.ffi.call(lib, "shim_apply_cmp", "i32", '
            '[Cmp, "i32", "i32"], [inner, a, b]);\n'
            "});\n"
            'std.ffi.call(lib, "shim_apply_cmp", "i32", '
            '[Cmp, "i32", "i32"], [outer, 9, 4])\n'
            "std.ffi.close(lib);\n",
            stdout="= 5",
        )
    check(
        "native module functions and constants",
        "import example.math;\n"
        "math.add(2, 3)\n"
        'math.join("a", "b")\n'
        "math.ANSWER\n"
        "math.PI\n"
        "math.NAME\n",
        stdout_all=["= 5", "= ab", "= 42", "= 3.14159", "= example"],
    )
    check(
        "native module userdata",
        "import example.math;\n"
        "let c = math.counter_new(5);\n"
        "c.value\n"
        "c.value = 9;\n"
        "c.value\n"
        "type_name(c)\n"
        "print(c)\n",
        stdout_all=["= 5", "= 9", "= Counter", "<Counter>"],
    )
    check(
        "native module packed and bitfield",
        "import example.math;\n"
        "let h = math.packed_header_new(7, 0x11223344, 0xABCD);\n"
        "h.tag\n"
        "h.length\n"
        "h.flags\n"
        "math.packed_header_size()\n"
        "h.length = 5;\n"
        "h.length\n"
        "let f = math.packed_flags_new(1, 5, 9);\n"
        "f.a\n"
        "f.b\n"
        "f.c\n"
        "f.b = 7;\n"
        "f.b\n"
        "type_name(h)\n",
        stdout_all=["= 7", "= 287454020", "= 43981", "= 7", "= 5",
                    "= 1", "= 5", "= 9", "= 7", "= PackedHeader"],
    )
    check(
        "native module import alias",
        "import example.math as m;\nm.ANSWER\n",
        stdout="= 42",
    )
    check(
        "native module argument error",
        "import example.math;\nmath.add(1)\n",
        stderr="add expects 2 arguments",
    )
    check(
        "native module type error",
        'import example.math;\nmath.add("x", 1)\n',
        stderr="add expects integers",
    )
    check(
        "native module field error",
        "import example.math;\nlet c = math.counter_new();\nc.missing\n",
        stderr="Counter has no such field",
    )
    check(
        "native module unknown",
        "import example.nope;\n",
        stderr="package not found",
    )
    if has_ffi and FFI_SHIM:
        check(
            "extern lib binding",
            'extern "c" lib sample from ' + shim + ' {\n'
            "  type Point {\n"
            "    x: f64;\n"
            "    y: f64;\n"
            "  }\n"
            "  func shim_point_sum(p: Point) => f64;\n"
            "  func shim_point_make(x: f64, y: f64) => Point;\n"
            "  const ANSWER: i32 = 42;\n"
            "}\n"
            'sample.shim_point_sum({"x": 1.0, "y": 2.0})\n'
            "let made = sample.shim_point_make(3.0, 4.0);\n"
            "made.y\n"
            "sample.ANSWER\n",
            stdout_all=["= 3.0", "= 4.0", "= 42"],
        )
        check(
            "extern lib callback",
            "import std;\n"
            'extern "c" lib sample from ' + shim + ' {\n'
            "  callback Cmp(a: i32, b: i32) => i32;\n"
            "  func shim_apply_cmp(cmp: Cmp, a: i32, b: i32) => i32;\n"
            "}\n"
            'let Cmp = std.ffi.type(sample, "Cmp");\n'
            "let cb = std.ffi.callback(Cmp, func(a, b) { return a - b; });\n"
            "sample.shim_apply_cmp(cb, 9, 4)\n",
            stdout="= 5",
        )
        check(
            "extern lib ownership annotations",
            "import std;\n"
            'extern "c" lib sample from ' + shim + ' {\n'
            "  type Point { x: f64; y: f64; }\n"
            "  type PointPtr = *Point;\n"
            "  callback Cmp(a: i32, b: i32) => i32;\n"
            '  func shim_alloc_point() => PointPtr owns "shim_free_point";\n'
            "  func shim_free_count() => i32;\n"
            "  func shim_save_cmp(cb: Cmp) retains cb;\n"
            "  func shim_call_saved(a: i32, b: i32) => i32;\n"
            "}\n"
            "let owned = sample.shim_alloc_point();\n"
            "std.ffi.deref(owned).x\n"
            "owned = null;\n"
            "sample.shim_free_count()\n"
            'let Cmp = std.ffi.type(sample, "Cmp");\n'
            "let cb = std.ffi.callback(Cmp, func(a, b) { return a + b; });\n"
            "sample.shim_save_cmp(cb);\n"
            "cb = null;\n"
            "sample.shim_call_saved(3, 4)\n",
            stdout_all=["= 1.0", "= 1", "= 7"],
        )
        check(
            "extern lib consumes annotation",
            "import std;\n"
            'extern "c" lib sample from ' + shim + ' {\n'
            "  type Point { x: f64; y: f64; }\n"
            "  type PointPtr = *Point;\n"
            "  func shim_consume_point(p: PointPtr) consumes p;\n"
            "}\n"
            'let Point = std.ffi.type(sample, "Point");\n'
            "let raw = std.ffi.alloc(Point);\n"
            "sample.shim_consume_point(raw);\n"
            "std.ffi.deref(raw)\n",
            stderr="consumed by C",
        )
        check(
            "extern lib unknown annotation parameter",
            'extern "c" lib sample from ' + shim + ' {\n'
            "  func shim_i32(value: i32) consumes missing;\n"
            "}\n",
            stderr="unknown parameter",
        )
        check(
            "extern lib manifest error",
            'extern "c" lib sample from ' + shim + ' {\n'
            "  type Empty {\n"
            "  }\n"
            "}\n",
            stderr="must have at least one field",
        )
    if NATIVE_MODULE:
        native_module = skein_string(NATIVE_MODULE)
        check(
            "native module dynamic load",
            "import std;\n"
            f"let name = std.module.load({native_module});\n"
            "name\n"
            "import test.dynamic;\n"
            "dynamic.twice(21)\n",
            stdout_all=["= test.dynamic", "= 42"],
        )
        check(
            "native module dynamic load error",
            'import std;\nstd.module.load("/definitely/not/a/module.so")\n',
            stderr="std.module.load",
        )
        check(
            "native module v1 additions",
            "import std;\n"
            f"std.module.load({native_module});\n"
            "import test.dynamic;\n"
            "let c = dynamic.counter_new(7);\n"
            "dynamic.counter_value(c)\n"
            "dynamic.list_pair(3)[1]\n"
            'dynamic.dict_value(4)["value"]\n',
            stdout_all=["= 7", "= 6", "= 4"],
        )
        check(
            "native module unload keeps live bindings",
            "import std;\n"
            f"std.module.load({native_module});\n"
            "import test.dynamic;\n"
            "dynamic.twice(21)\n"
            'std.module.unload("test.dynamic");\n'
            "dynamic.twice(21)\n",
            stdout_all=["= 42", "= 42"],
        )
        check(
            "native module unload removes import",
            "import std;\n"
            f"std.module.load({native_module});\n"
            'std.module.unload("test.dynamic");\n'
            "import test.dynamic;\n",
            stderr="package not found",
        )
        check(
            "native module cannot unload static",
            "import std;\n"
            'std.module.unload("example.math")\n',
            stderr="cannot unload a static native module",
        )
        with tempfile.TemporaryDirectory() as fini_dir:
            fini_path = os.path.join(fini_dir, "fini.txt")
            fini_env = dict(os.environ)
            fini_env["SKEIN_TEST_FINI_FILE"] = fini_path
            check(
                "native module fini",
                "import std;\n"
                f"std.module.load({native_module});\n"
                "func use() {\n"
                "  import test.dynamic;\n"
                "  return dynamic.twice(1);\n"
                "}\n"
                "use();\n"
                'std.module.unload("test.dynamic");\n',
                stdout="= 2",
                env=fini_env,
            )
            assert os.path.exists(fini_path), "module fini was not called"
            with open(fini_path, encoding="utf-8") as handle:
                assert handle.read() == "fini"
        if FFI_SHIM:
            check(
                "native module missing descriptor",
                "import std;\n"
                f"std.module.load({skein_string(FFI_SHIM)})\n",
                stderr="missing skein_module_descriptor",
            )
    import json as _json
    bindgen_header = os.path.normpath(os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        "..", "bindgen_sample.h"))
    with tempfile.TemporaryDirectory() as bindgen_dir:
        bindgen_output = os.path.join(bindgen_dir, "sample.ffi.json")
        proc = subprocess.run(
            [BIN, "--ffi-gen", "--header", bindgen_header,
             "--module", "sample", "-o", bindgen_output],
            text=True, capture_output=True, timeout=60)
        assert proc.returncode == 0, (proc.stdout, proc.stderr)
        with open(bindgen_output, encoding="utf-8") as handle:
            generated = _json.load(handle)
        assert generated["library"] == "sample"
        assert generated["types"]["SamplePoint"]["kind"] == "struct"
        assert generated["types"]["SampleCompare"]["kind"] == "callback"
        hooks = generated["types"]["SampleHooks"]["fields"]
        assert hooks[0]["type"] == "SampleCompare"
        assert hooks[1]["type"]["kind"] == "callback"
        assert hooks[1]["type"]["args"] == ["cstring"]
        assert generated["functions"]["sample_add"]["result"] == "i32"
        assert generated["functions"]["sample_varargs"]["variadic"] is True
        assert (generated["functions"]["sample_stdcall"]
                ["calling_convention"] == "stdcall")
        assert (generated["functions"]["sample_sort"]["args"][2]
                == "SampleCompare")
        assert generated["constants"]["SAMPLE_MAX"] == 16
        assert generated["constants"]["SAMPLE_SHIFT"] == 16
        assert generated["constants"]["SAMPLE_MASK"] == 9
        assert generated["constants"]["SAMPLE_COMPARE"] == 1
        assert generated["constants"]["SAMPLE_LOGICAL"] == 0
        assert generated["constants"]["SAMPLE_TERNARY"] == 7
        assert generated["constants"]["SAMPLE_CAST"] == 255
        assert generated["constants"]["SAMPLE_BINARY"] == 5
        assert generated["constants"]["SAMPLE_FLOAT_SUFFIX"] == 2.5
        assert generated["variables"] == {
            "sample_global": "i32",
            "sample_const": {"type": "i32", "const": True},
        }
        assert generated["constants"]["SAMPLE_KIND_A"] == 4
        assert generated["constants"]["SAMPLE_KIND_B"] == 5
        assert generated["constants"]["SAMPLE_KIND_C"] == 6
        assert generated["constants"]["SAMPLE_KIND_D"] == 7

        check_proc = subprocess.run(
            [BIN, "--ffi-gen", "--header", bindgen_header,
             "--module", "sample", "--check", "-o", bindgen_output],
            text=True, capture_output=True, timeout=60)
        assert check_proc.returncode == 0, (check_proc.stdout,
                                            check_proc.stderr)

        filtered_output = os.path.join(bindgen_dir, "filtered.ffi.json")
        filter_proc = subprocess.run(
            [BIN, "--ffi-gen", "--header", bindgen_header,
             "--symbols", "sample_add", "-o", filtered_output],
            text=True, capture_output=True, timeout=60)
        assert filter_proc.returncode == 0, filter_proc.stderr
        with open(filtered_output, encoding="utf-8") as handle:
            filtered = _json.load(handle)
        assert list(filtered["functions"]) == ["sample_add"]
        assert filtered["variables"] == {}
        assert filtered["types"] == {}

        cpp_header = os.path.normpath(os.path.join(
            os.path.dirname(os.path.abspath(__file__)),
            "..", "bindgen_cpp.h"))
        cpp = subprocess.run(
            [BIN, "--ffi-gen", "--header", cpp_header],
            text=True, capture_output=True, timeout=60)
        assert cpp.returncode == 1
        assert "C++ headers are not supported" in cpp.stderr

        aligned_header = os.path.join(bindgen_dir, "aligned.h")
        with open(aligned_header, "w", encoding="utf-8") as handle:
            handle.write(
                "struct __attribute__((aligned(16))) Aligned { int x; };\n")
        aligned = subprocess.run(
            [BIN, "--ffi-gen", "--header", aligned_header],
            text=True, capture_output=True, timeout=60)
        assert aligned.returncode == 1
        assert "layout-changing attribute" in aligned.stderr

        packed_header = os.path.join(bindgen_dir, "packed.h")
        with open(packed_header, "w", encoding="utf-8") as handle:
            handle.write(
                "struct Packed { int x; } __attribute__((packed));\n")
        packed = subprocess.run(
            [BIN, "--ffi-gen", "--header", packed_header],
            text=True, capture_output=True, timeout=60)
        assert packed.returncode == 1
        assert "packed structs are not supported" in packed.stderr

        strict_header = os.path.normpath(os.path.join(
            os.path.dirname(os.path.abspath(__file__)),
            "..", "bindgen_strict.h"))
        strict = subprocess.run(
            [BIN, "--ffi-gen", "--header", strict_header],
            text=True, capture_output=True, timeout=60)
        assert strict.returncode == 1, (strict.stdout, strict.stderr)
        for needle in (
                "unsupported calling convention",
                "long double is not supported",
                "unknown type 'unknown_type'",
                "unknown type '_Alignas'",
                "thread-local variables are not supported"):
            assert needle in strict.stderr, (needle, strict.stderr)

        tolerated = subprocess.run(
            [BIN, "--ffi-gen", "--header", strict_header,
             "--tolerate-errors"],
            text=True, capture_output=True, timeout=60)
        assert tolerated.returncode == 0, (tolerated.stdout,
                                           tolerated.stderr)
        assert "warning:" in tolerated.stderr

        types_header = os.path.normpath(os.path.join(
            os.path.dirname(os.path.abspath(__file__)),
            "..", "bindgen_types.h"))
        types_output = os.path.join(bindgen_dir, "types.ffi.json")
        types_gen = subprocess.run(
            [BIN, "--ffi-gen", "--header", types_header,
             "-o", types_output],
            text=True, capture_output=True, timeout=60)
        assert types_gen.returncode == 0, (types_gen.stdout,
                                           types_gen.stderr)
        with open(types_output, encoding="utf-8") as handle:
            generated_types = _json.load(handle)
        byte_args = generated_types["functions"]["type_bytes"]["args"]
        assert byte_args == ["bytes", "bytes", "i32"]
        assert (generated_types["functions"]["type_text"]["args"][0]
                == "cstring")
        assert generated_types["functions"]["type_array"]["args"][0] == {
            "kind": "pointer", "to": "i32"}
        assert generated_types["functions"]["type_matrix"]["args"][0] == {
            "kind": "pointer",
            "to": {"kind": "array", "element": "i32", "count": 4}}

        dep_header = os.path.normpath(os.path.join(
            os.path.dirname(os.path.abspath(__file__)),
            "..", "bindgen_dep_main.h"))
        dep_output = os.path.join(bindgen_dir, "dep.ffi.json")
        dep_proc = subprocess.run(
            [BIN, "--ffi-gen", "--header", dep_header,
             "-o", dep_output],
            text=True, capture_output=True, timeout=60)
        assert dep_proc.returncode == 0, (dep_proc.stdout, dep_proc.stderr)
        with open(dep_output, encoding="utf-8") as handle:
            generated_dep = _json.load(handle)
        assert "DepPoint" in generated_dep["types"]
        assert (generated_dep["types"]["DepPoint"]["kind"] == "struct")
        assert (generated_dep["functions"]["dep_make_point"]["args"][0]
                == "u64")
        assert (generated_dep["functions"]["dep_set_mode"]["args"][0]
                == "i32")

        dep_isolated = subprocess.run(
            [BIN, "--ffi-gen", "--header", dep_header,
             "--no-include-types"],
            text=True, capture_output=True, timeout=60)
        assert dep_isolated.returncode == 1, (dep_isolated.stdout,
                                              dep_isolated.stderr)
        assert "unknown type" in dep_isolated.stderr

        opaque_header = os.path.normpath(os.path.join(
            os.path.dirname(os.path.abspath(__file__)),
            "..", "bindgen_opaque.h"))
        opaque_strict = subprocess.run(
            [BIN, "--ffi-gen", "--header", opaque_header],
            text=True, capture_output=True, timeout=60)
        assert opaque_strict.returncode == 1, (opaque_strict.stdout,
                                                opaque_strict.stderr)

        opaque_output = os.path.join(bindgen_dir, "opaque.ffi.json")
        opaque = subprocess.run(
            [BIN, "--ffi-gen", "--header", opaque_header,
             "--allow-opaque-types", "-o", opaque_output],
            text=True, capture_output=True, timeout=60)
        assert opaque.returncode == 0, (opaque.stdout, opaque.stderr)
        assert "by-value opaque type" in opaque.stderr
        with open(opaque_output, encoding="utf-8") as handle:
            generated_opaque = _json.load(handle)
        assert generated_opaque["types"]["OpaqueRecord"] == {
            "kind": "opaque"}
        assert "opaque_record_new" in generated_opaque["functions"]
        assert "opaque_record_read" in generated_opaque["functions"]
        assert "opaque_record_take" not in generated_opaque["functions"]

        extern_c_header = os.path.normpath(os.path.join(
            os.path.dirname(os.path.abspath(__file__)),
            "..", "bindgen_extern_c.h"))
        extern_c_output = os.path.join(bindgen_dir, "extern_c.ffi.json")
        extern_c = subprocess.run(
            [BIN, "--ffi-gen", "--header", extern_c_header,
             "-o", extern_c_output],
            text=True, capture_output=True, timeout=60)
        assert extern_c.returncode == 0, (extern_c.stdout, extern_c.stderr)
        with open(extern_c_output, encoding="utf-8") as handle:
            generated_extern_c = _json.load(handle)
        assert (generated_extern_c["functions"]["extern_c_add"]["args"]
                == ["i32", "i32"])

        if has_ffi and FFI_SHIM:
            good_manifest = os.path.join(bindgen_dir, "shim_good.ffi.json")
            with open(good_manifest, "w", encoding="utf-8") as handle:
                _json.dump({
                    "ffi": 1,
                    "functions": {
                        "shim_i32": {"result": "i32", "args": ["i32"]},
                    },
                }, handle)
            good = subprocess.run(
                [BIN, "--check-ffi", FFI_SHIM, good_manifest],
                text=True, capture_output=True, timeout=60)
            assert good.returncode == 0, (good.stdout, good.stderr)

            bad_manifest = os.path.join(bindgen_dir, "shim_bad.ffi.json")
            with open(bad_manifest, "w", encoding="utf-8") as handle:
                _json.dump({
                    "ffi": 1,
                    "functions": {"nope": {"result": "i32"}},
                }, handle)
            bad = subprocess.run(
                [BIN, "--check-ffi", FFI_SHIM, bad_manifest],
                text=True, capture_output=True, timeout=60)
            assert bad.returncode == 1, (bad.stdout, bad.stderr)
            assert "missing symbol: nope" in bad.stderr

            missing_free = os.path.join(bindgen_dir, "shim_no_free.ffi.json")
            with open(missing_free, "w", encoding="utf-8") as handle:
                _json.dump({
                    "ffi": 1,
                    "functions": {
                        "shim_alloc_point": {
                            "result": "ptr",
                            "returns_owned": {"free": "no_such_free"},
                        },
                    },
                }, handle)
            no_free = subprocess.run(
                [BIN, "--check-ffi", FFI_SHIM, missing_free],
                text=True, capture_output=True, timeout=60)
            assert no_free.returncode == 1, (no_free.stdout, no_free.stderr)
            assert "missing symbol: no_such_free" in no_free.stderr

            variables_manifest = os.path.join(bindgen_dir,
                                              "shim_variables.ffi.json")
            with open(variables_manifest, "w", encoding="utf-8") as handle:
                _json.dump({
                    "ffi": 1,
                    "variables": {"shim_global": {"type": "i32"}},
                }, handle)
            variables = subprocess.run(
                [BIN, "--check-ffi", FFI_SHIM, variables_manifest],
                text=True, capture_output=True, timeout=60)
            assert variables.returncode == 0, (variables.stdout,
                                               variables.stderr)

            missing_variable = os.path.join(bindgen_dir,
                                            "shim_no_variable.ffi.json")
            with open(missing_variable, "w", encoding="utf-8") as handle:
                _json.dump({
                    "ffi": 1,
                    "variables": {"nope_global": {"type": "i32"}},
                }, handle)
            no_variable = subprocess.run(
                [BIN, "--check-ffi", FFI_SHIM, missing_variable],
                text=True, capture_output=True, timeout=60)
            assert no_variable.returncode == 1, (no_variable.stdout,
                                                 no_variable.stderr)
            assert "missing symbol: nope_global" in no_variable.stderr

        if has_ffi:
            check(
                "ffi bindgen manifest validates",
                "import std;\n"
                f'std.ffi.manifest({skein_string(bindgen_output)})\n',
                stdout="= <ffi_manifest sample>",
            )
    check(
        "std string format",
        'import std;\nstd.string.format("{}", 3)\n',
        stdout="= 3",
    )
    check(
        "std list helpers",
        "import std;\n"
        "std.list.len([1, 2, 3])\n"
        "std.list.slice([1, 2, 3, 4], 1, 2)\n"
        "std.list.contains([1, 2, 3], 2)\n"
        "std.list.index([1, 2, 3], 3)\n",
        stdout_all=["= 3", "= [2, 3]", "= 1", "= 2"],
    )
    check(
        "std list slice default end",
        "import std;\n"
        "std.list.slice([1, 2, 3, 4], 1)\n"
        "kind_of(std.list.slice)\n"
        "kind_of(std.list.append)\n",
        stdout_all=["= [2, 3, 4]", "= function", "= builtin"],
    )
    check(
        "std list import",
        "import std.list;\n"
        "list.slice([1, 2, 3, 4], 1)\n"
        "list.append([1, 2], 3)\n",
        stdout_all=["= [2, 3, 4]", "= [1, 2, 3]"],
    )
    check(
        "std dict import",
        "import std.dict;\n"
        'dict.get({"a": 1}, "a")\n',
        stdout="= 1",
    )
    check(
        "std container adapter defaults",
        "import std;\n"
        'bytes_to_string(std.bytes.slice(bytes_from_string("abc"), 1))\n'
        "kind_of(std.list.sort)\n"
        "kind_of(std.list.reduce)\n"
        "kind_of(std.dict.get)\n"
        "kind_of(std.lru.get)\n"
        "kind_of(std.bytes.slice)\n",
        stdout_all=["= bc", "= function", "= function", "= function",
                    "= function", "= function"],
    )
    check(
        "std container adapter bindings",
        "import std;\n"
        "kind_of(std.list.slice)\n"
        "kind_of(std.list.sort)\n"
        "kind_of(std.list.sort_by)\n"
        "kind_of(std.list.is_sorted)\n"
        "kind_of(std.list.top_k)\n"
        "kind_of(std.list.binary_search)\n"
        "kind_of(std.list.reduce)\n"
        "kind_of(std.dict.get)\n"
        "kind_of(std.lru.get)\n"
        "kind_of(std.lru.peek)\n"
        "kind_of(std.bytes.slice)\n"
        "kind_of(std.raw.list.slice)\n",
        stdout_all=["= function"] * 11 + ["= builtin"],
    )
    check(
        "std list slice clamps",
        "import std;\nstd.list.slice([1, 2], 1, 10)\n",
        stdout="= [2]",
    )
    check(
        "std dict helpers",
        "import std;\n"
        'std.dict.get({"a": 1}, "a")\n'
        'std.dict.get({"a": 1}, "b")\n'
        'std.dict.get({"a": 1}, "b", 9)\n'
        'std.dict.has({"a": 1}, "a")\n'
        'std.dict.keys({"a": 1, "b": 2})\n'
        'std.dict.values({"a": 1, "b": 2})\n',
        stdout_all=["= 1", "= 9", "= 1", "= [a, b]", "= [1, 2]"],
    )
    check(
        "std string helpers",
        "import std;\n"
        'std.string.join(["a", "b", "c"], ",")\n'
        'std.string.replace("a-b-c", "-", "_")\n'
        'std.string.starts_with("hello", "he")\n'
        'std.string.ends_with("hello", "lo")\n'
        'std.string.find("hello", "ll")\n'
        'std.string.trim("  x  ")\n',
        stdout_all=["= a,b,c", "= a_b_c", "= 1", "= 1", "= 2", "= x"],
    )
    check(
        "std bytes aliases",
        "import std;\n"
        'std.bytes.hex(bytes_from_hex("00ff"))\n'
        'std.bytes.len(bytes_from_hex("00ff"))\n',
        stdout_all=["= 00ff", "= 2"],
    )
    check(
        "std bytes string aliases",
        "import std;\n"
        'std.bytes.to_string(std.bytes.from_string("abc"))\n'
        'std.bytes.to_string(bytes_from_hex("ff"))\n',
        stdout_all=["= abc"],
        stderr="not valid UTF-8",
    )
    check(
        "std encoding gbk roundtrip",
        "import std;\n"
        'print(std.encoding.decode(bytes_from_hex("d6d0b9fa"), "gbk"));\n'
        'print(bytes_hex(std.encoding.encode("中国", "gbk")));\n'
        'std.encoding.supports("gbk")\n',
        stdout_all=["中国", "d6d0b9fa", "= true"],
    )
    check(
        "std encoding gbk euro",
        "import std;\n"
        'print(bytes_hex(std.encoding.encode("€", "gbk")));\n'
        'print(std.encoding.decode(bytes_from_hex("80"), "gbk"));\n',
        stdout_all=["80", "€"],
    )
    check(
        "std encoding gb18030 vectors",
        "import std;\n"
        'print(std.encoding.decode(bytes_from_hex("d6d0"), "gb18030"));\n'
        'print(bytes_hex(std.encoding.encode("€", "gb18030")));\n'
        'let s = std.encoding.decode(bytes_from_hex("90308130"), "gb18030");\n'
        'print(bytes_hex(std.encoding.encode(s, "gb18030")));\n'
        'print(bytes_hex(std.encoding.encode("😀", "gb18030")));\n'
        'std.encoding.supports("gb18030")\n',
        stdout_all=["中", "a2e3", "90308130", "9439fc36", "= true"],
    )
    check(
        "std encoding gb18030 invalid byte",
        "import std;\n"
        'std.encoding.decode(bytes_from_hex("80"), "gb18030")\n',
        stderr="invalid gb18030",
    )
    check(
        "std encoding utf16 bom",
        "import std;\n"
        'print(bytes_hex(std.encoding.encode("中", "utf-16")));\n'
        'print(std.encoding.decode(bytes_from_hex("fffe2d4e"), "utf-16"));\n',
        stdout_all=["fffe2d4e", "中"],
    )
    check(
        "std encoding utf32",
        "import std;\n"
        'print(bytes_hex(std.encoding.encode("中", "utf-32le")));\n'
        'print(bytes_hex(std.encoding.encode("中", "utf-32be")));\n'
        'print(bytes_hex(std.encoding.encode("中", "utf-32")));\n'
        'print(std.encoding.decode(bytes_from_hex("2d4e0000"), "utf-32le"));\n'
        'print(std.encoding.decode(bytes_from_hex("00004e2d"), "utf-32be"));\n'
        'print(std.encoding.decode(bytes_from_hex("fffe00002d4e0000"), '
        '"utf-32"));\n',
        stdout_all=["2d4e0000", "00004e2d", "fffe00002d4e0000",
                    "中", "中", "中"],
    )
    check(
        "std encoding error modes",
        "import std;\n"
        'print(std.encoding.decode(bytes_from_hex("ff"), "utf-8", "replace"));\n'
        'print(bytes_hex(std.encoding.encode("é", "ascii", "replace")));\n'
        'bytes_len(std.encoding.encode("é", "ascii", "ignore"))\n',
        stdout_all=["�", "3f", "= 0"],
    )
    check(
        "std encoding nul rejected",
        "import std;\n"
        'std.encoding.decode(bytes_from_hex("0000"), "utf-16le")\n',
        stderr="NUL",
    )
    check(
        "std encoding unknown name",
        "import std;\n"
        'std.encoding.supports("not-an-encoding")\n'
        'std.encoding.decode(bytes_from_hex("00"), "not-an-encoding")\n',
        stdout_all=["= false"],
        stderr="unknown encoding",
    )
    check(
        "std encoding source type",
        "import std;\n"
        'std.encoding.decode("abc", "gbk")\n',
        stderr="expects bytes",
    )
    check(
        "std unicode normalization forms",
        "import std;\n"
        'let decomposed = std.encoding.decode(bytes_from_hex("65cc81"), "utf-8");\n'
        'print(bytes_hex(std.encoding.encode('
        'std.unicode.normalize(decomposed, "nfc"), "utf-8")));\n'
        'let composed = std.encoding.decode(bytes_from_hex("c3a9"), "utf-8");\n'
        'print(bytes_hex(std.encoding.encode('
        'std.unicode.normalize(composed, "nfd"), "utf-8")));\n'
        'let circled = std.encoding.decode(bytes_from_hex("e291a0"), "utf-8");\n'
        'print(std.unicode.normalize(circled, "nfkc"));\n'
        'print(std.unicode.normalize(circled, "nfkd"));\n'
        'std.unicode.version()\n',
        stdout_all=["c3a9", "65cc81", "1", "1", "= 16.0.0"],
    )
    check(
        "std unicode is normalized",
        "import std;\n"
        'let decomposed = std.encoding.decode(bytes_from_hex("65cc81"), "utf-8");\n'
        'let composed = std.encoding.decode(bytes_from_hex("c3a9"), "utf-8");\n'
        'std.unicode.is_normalized(decomposed, "nfc")\n'
        'std.unicode.is_normalized(composed, "nfc")\n'
        'std.unicode.is_normalized(composed, "nfd")\n',
        stdout_all=["= false", "= true", "= false"],
    )
    check(
        "std unicode compatibility",
        "import std;\n"
        'let ligature = std.encoding.decode(bytes_from_hex("efac81"), "utf-8");\n'
        'print(bytes_hex(std.encoding.encode('
        'std.unicode.normalize(ligature, "nfc"), "utf-8")));\n'
        'print(bytes_hex(std.encoding.encode('
        'std.unicode.normalize(ligature, "nfkc"), "utf-8")));\n'
        'let fullwidth = std.encoding.decode(bytes_from_hex("efbc91"), "utf-8");\n'
        'print(std.unicode.normalize(fullwidth, "nfkc"));\n',
        stdout_all=["efac81", "6669", "1"],
    )
    check(
        "std unicode hangul",
        "import std;\n"
        'let jamo = std.encoding.decode(bytes_from_hex("e18480e185a1"), "utf-8");\n'
        'print(bytes_hex(std.encoding.encode('
        'std.unicode.normalize(jamo, "nfc"), "utf-8")));\n',
        stdout_all=["eab080"],
    )
    check(
        "std unicode canonical order",
        "import std;\n"
        'let angstrom = std.encoding.decode(bytes_from_hex("e284ab"), "utf-8");\n'
        'print(bytes_hex(std.encoding.encode('
        'std.unicode.normalize(angstrom, "nfc"), "utf-8")));\n'
        'print(bytes_hex(std.encoding.encode('
        'std.unicode.normalize(angstrom, "nfd"), "utf-8")));\n'
        'let marks = std.encoding.decode(bytes_from_hex("61cc95cc80"), "utf-8");\n'
        'print(bytes_hex(std.encoding.encode('
        'std.unicode.normalize(marks, "nfd"), "utf-8")));\n',
        stdout_all=["c385", "41cc8a", "61cc80cc95"],
    )
    check(
        "std unicode unknown form",
        "import std;\n"
        'std.unicode.normalize("x", "invalid")\n',
        stderr="unknown normalization form",
    )
    check(
        "std unicode source type",
        "import std;\n"
        'std.unicode.normalize(1, "nfc")\n',
        stderr="expects a string",
    )
    check("std join type", 'import std;\nstd.string.join([1], ",")\n',
          stderr="expects string elements")
    check("std replace empty", 'import std;\nstd.string.replace("a", "", "b")\n',
          stderr="search must not be empty")
    check("std list slice type", "import std;\nstd.list.slice(1, 0)\n",
          stderr="std.list.slice expects a list")
    check("std list slice start", "import std;\nstd.list.slice([1], -1)\n",
          stderr="start must be a non-negative integer")
    check("std dict get type", 'import std;\nstd.dict.get([], "a")\n',
          stderr="std.dict.get expects a dict")
    check("std import missing module", "std.list.len([1])\n",
          stderr="symbol not found std")
    check(
        "std namespace copy is isolated",
        "import std;\n"
        "func mutate(ns) { ns.list = 1; }\n"
        "mutate(std);\n"
        "std.list.len([1, 2])\n",
        stdout="= 2",
    )
    check(
        "std list transformations",
        "import std;\n"
        "std.list.reverse([1, 2, 3])\n"
        "std.list.sort([3, 1, 2])\n"
        "std.list.sort([2.5, 1, 3])\n"
        'std.list.sort(["b", "a", "c"])\n'
        "std.list.append([1, 2], 3)\n",
        stdout_all=[
            "= [3, 2, 1]",
            "= [1, 2, 3]",
            "= [1, 2.5, 3]",
            "= [a, b, c]",
            "= [1, 2, 3]",
        ],
    )
    check(
        "std list functional helpers",
        "import std;\n"
        "std.list.map([1, 2, 3], func(x) { return x * 2; })\n"
        "std.list.filter([1, 2, 3, 4], func(x) { return x % 2 == 0; })\n"
        "std.list.reduce([1, 2, 3, 4], func(a, b) { return a + b; })\n"
        "std.list.reduce([1, 2, 3, 4], func(a, b) { return a + b; }, 10)\n",
        stdout_all=["= [2, 4, 6]", "= [2, 4]", "= 10", "= 20"],
    )
    check(
        "std dict items/delete/merge",
        "import std;\n"
        'std.dict.items({"a": 1, "b": 2})\n'
        'std.dict.delete({"a": 1, "b": 2}, "a")\n'
        'std.dict.delete({"a": 1}, "missing")\n'
        'std.dict.merge({"a": 1, "b": 2}, {"b": 9, "c": 3})\n',
        stdout_all=["= [[a, 1], [b, 2]]", "= {b: 2}", "= {a: 1}",
                    "= {a: 1, b: 9, c: 3}"],
    )
    check(
        "std string extras",
        "import std;\n"
        'std.string.contains("hello", "ell")\n'
        'std.string.contains("hello", "x")\n'
        'std.string.repeat("ab", 3)\n'
        'std.string.repeat("ab", 0) == ""\n'
        'std.string.trim_start("  x  ")\n'
        'std.string.trim_end("  x  ")\n',
        stdout_all=["= 1", "= 0", "= ababab", "= true", "= x  ", "=   x"],
    )
    check(
        "std list sort type error",
        'import std;\nstd.list.sort([1, "a"])\n',
        stderr="expects all numbers, strings, chars, or bytes",
    )
    check(
        "std reduce empty error",
        "import std;\nstd.list.reduce([], func(a, b) { return a + b; })\n",
        stderr="empty list without initial value",
    )
    check(
        "std map callback error",
        "import std;\n"
        "std.list.map([1], func(x) { return 1/0; })\n",
        stderr="division by zero",
    )
    check(
        "std repeat negative",
        'import std;\nstd.string.repeat("a", -1)\n',
        stderr="repeat count must be non-negative",
    )
    check(
        "std dict merge type",
        "import std;\nstd.dict.merge({\"a\": 1}, 1)\n",
        stderr="std.dict.merge expects two dicts",
    )
    check(
        "std json parse",
        r'''import std;
    let v = std.json.parse("{\"a\": 1, \"b\": [true, null, \"x\"], \"c\": 1.5}");
    print(v["a"]);
    print(v["b"][0]);
    print(v["b"][1]);
    print(v["b"][2]);
    print(v["c"]);
    ''',
        stdout_all=["1", "true", "null", "x", "1.5"],
    )
    check(
        "std json stringify",
        r'''import std;
    std.json.stringify({"a": 1, "b": [true, null, "x"], "c": 1.5})
    std.json.stringify({"s": "a\"b\n\\c"})
    std.json.stringify(1.0)
    ''',
        stdout_all=[r'= {"a":1,"b":[true,null,"x"],"c":1.5}',
                    r'= {"s":"a\"b\n\\c"}', "= 1.0"],
    )
    check(
        "std json numbers",
        r'''import std;
    std.type.kind_of(std.json.parse("1"))
    std.type.kind_of(std.json.parse("1.0"))
    std.type.kind_of(std.json.parse("9223372036854775808"))
    std.json.parse("-9223372036854775808")
    ''',
        stdout_all=["= int", "= float", "= float",
                    "= -9223372036854775808"],
    )
    check(
        "std json duplicate keys and unicode",
        r'''import std;
    std.json.parse("{\"a\":1,\"a\":2}")["a"]
    std.json.parse("\"\\u4f60\\u597d\"")
    ''',
        stdout_all=["= 2", "= 你好"],
    )
    check(
        "std json roundtrip",
        r'''import std;
    let text = "{\"a\": [1, 2], \"b\": {\"c\": true}, \"s\": \"\\u4f60\"}";
    std.json.stringify(std.json.parse(text))
    ''',
        stdout=r'= {"a":[1,2],"b":{"c":true},"s":"你"}',
    )
    check(
        "std json try_parse",
        'import std;\n'
        'let r = std.json.try_parse("{bad}");\n'
        'std.result.is_err(r)\n'
        'std.result.is_ok(std.json.try_parse("{\\"a\\":1}"))\n',
        stdout_all=["= true", "= true"],
    )
    check(
        "std json parse error",
        'import std;\nstd.json.parse("{\\"a\\":1")\n',
        stderr="JSON parse error",
    )
    check(
        "std json parse bytes",
        'import std;\n'
        'std.json.parse(bytes_from_string("{\\"a\\": 7}"))["a"]\n',
        stdout="= 7",
    )
    check(
        "std json parse type",
        "import std;\nstd.json.parse(1)\n",
        stderr="std.json.parse expects a string or bytes",
    )
    check(
        "std json parse invalid utf8",
        'import std;\nstd.json.parse(bytes_from_hex("ff"))\n',
        stderr="not valid UTF-8",
    )
    check(
        "std json stringify key error",
        "import std;\nstd.json.stringify({1: 2})\n",
        stderr="JSON object keys must be strings",
    )
    check(
        "std json stringify unsupported",
        'import std;\nstd.json.stringify({"a": bytes_from_hex("00")})\n',
        stderr="cannot encode bytes as JSON",
    )

    check(
        "break",
        "let i = 0;\n"
        "while (i < 10) { i = i + 1; if (i == 3) { break; } print(i); }\n",
        stdout="1\n2\n",
    )
    check(
        "continue",
        "let i = 0;\n"
        "while (i < 5) { i = i + 1; if (i == 2) { continue; } print(i); }\n",
        stdout="1\n3\n",
    )
    check(
        "labeled break outer loop",
        "label outer: for (let i in [1, 2, 3]) {\n"
        "  for (let j in [1, 2, 3]) {\n"
        "    if (i == 2 && j == 2) { break outer; }\n"
        "    print(str(i) + \",\" + str(j));\n"
        "  }\n"
        "}\n"
        'print("done");\n',
        stdout_all=["1,1", "1,2", "1,3", "2,1", "done"],
    )
    check(
        "labeled continue outer loop",
        "label outer: for (let i in [1, 2]) {\n"
        "  for (let j in [1, 2]) {\n"
        "    if (j == 1) { continue outer; }\n"
        "    print(\"bad\");\n"
        "  }\n"
        "}\n"
        'print("done");\n',
        stdout="done\n",
    )
    check(
        "labeled break switch",
        "label sw: switch (1) {\n"
        "  case 1 => { break sw; }\n"
        '  default => print("bad");\n'
        "}\n"
        'print("after");\n',
        stdout="after\n",
    )
    check(
        "label on non-loop rejected",
        "label bad: if (1) { print(1); }\n",
        stderr="label must be attached to a loop or switch",
    )
    check(
        "block scope",
        "let x = 1;\n{ let x = 2; }\nx\n",
        stdout="= 1",
    )
    check(
        "block assignment",
        "let x = 1;\n{ x = 2; }\nx\n",
        stdout="= 2",
    )

    check("fail caught with recover",
          'func f() { fail "boom"; }\n'
          "f() on error(e) { print(e); recover 0; }\n",
          stdout_all=["boom", "= 0"])
    check("on error handler not called on success",
          "func f() { return 1; }\n"
          'f() on error(e) { print("bad"); recover 0; }\n',
          stdout="= 1")
    check("pass propagates to outer handler",
          'func inner() { fail "x"; }\n'
          "func outer() { return inner() on error(e) { pass e; }; }\n"
          'outer() on error(e) { print("caught " + str(e)); recover 7; }\n',
          stdout_all=["caught x", "= 7"])
    check("fail stops loop body",
          "func f() {\n"
          "  for (let i in [1, 2, 3]) {\n"
          '    if (i == 2) { fail "loop"; }\n'
          "    print(i);\n"
          "  }\n"
          "  return 0;\n"
          "}\n"
          'f() on error(e) { print("E " + str(e)); recover 9; }\n',
          stdout_all=["1", "E loop", "= 9"])
    check("recover value is expression result",
          'func f() { fail "x"; }\n'
          "(f() on error(e) { recover 2; }) + 3\n",
          stdout="= 5")
    check("handler accesses outer scope",
          'let n = 3;\nfunc f() { fail "x"; }\n'
          "f() on error(e) { recover n + 1; }\n",
          stdout="= 4")
    check("recover outside handler",
          "recover 1;\n", stderr="recover outside error handler")
    check("pass outside handler",
          "pass 1;\n", stderr="pass outside error handler")
    check("handler without terminal statement",
          'func f() { fail "x"; }\n'
          "let x = f() on error(e) { print(e); };\n",
          stderr="error handler must end with recover, pass, or fail")
    check("handler return rejected",
          'func f() { fail "x"; }\n'
          "func g() { let x = f() on error(e) { return 1; }; return 2; }\n"
          "g()\n",
          stderr="error handler cannot return, break, or continue")
    check("method fail caught",
          "struct P {\n"
          "  x: int;\n"
          '  func check(self) { if (self.x < 0) { fail "negative"; } '
          "return self.x; }\n"
          "}\n"
          'P { x: -1 }.check() on error(e) { print(e); recover 0; }\n',
          stdout_all=["negative", "= 0"])
    check("recover does not cross function boundary",
          'func bad() { recover 1; }\n'
          'func f() { fail "x"; }\n'
          "f() on error(e) { bad(); recover 0; };\n",
          stderr="recover outside error handler")
    check("uncaught fail at top level",
          'fail "boom";\n', stderr="uncaught fail: boom")

    check("variadic function",
          "func sum(...nums: number) {\n"
          "  let total = 0;\n"
          "  for (let n in nums) { total = total + n; }\n"
          "  return total;\n"
          "}\n"
          "sum()\nsum(1, 2, 3)\n",
          stdout_all=["= 0", "= 6"])
    check("variadic with fixed parameters",
          "func f(a: int, ...rest: string) {\n"
          "  return a + len(rest);\n"
          "}\n"
          "f(1)\nf(1, \"a\", \"b\")\n",
          stdout_all=["= 1", "= 3"])
    check("variadic rest is list",
          "func f(...xs) {\n"
          "  if (len(xs) == 0) { return null; }\n"
          "  return xs[1];\n"
          "}\n"
          "f()\nf(10, 20)\n",
          stdout_all=["= 20"])
    check("variadic arity",
          "func f(a: int, ...rest) { return a; }\nf()\n",
          stderr="function f expects at least 1 argument(s), got 0")
    check("variadic element type",
          'func f(...rest: number) { return len(rest); }\nf(1, "x")\n',
          stderr="function f rest argument 2 expects number, got string")
    check("variadic must be last",
          "func f(...xs, y) { return y; }\n",
          stderr="variadic parameter must be last")
    check("variadic method",
          "func sum(...nums: number) {\n"
          "  let total = 0;\n"
          "  for (let n in nums) { total = total + n; }\n"
          "  return total;\n"
          "}\n"
          "struct P { x: int; }\n"
          "impl P {\n"
          "  func add(self, ...nums: int) { return sum(...nums); }\n"
          "}\n"
          "P { x: 1 }.add(1, 2, 3)\n",
          stdout="= 6")
    check("variadic anonymous function",
          "func make() { return func(...args: int) { return len(args); }; }\n"
          "make()(1, 2, 3)\n",
          stdout="= 3")
    check("call spread",
          "func sum(...nums: number) {\n"
          "  let total = 0;\n"
          "  for (let n in nums) { total = total + n; }\n"
          "  return total;\n"
          "}\n"
          "let xs = [1, 2, 3];\n"
          "sum(...xs)\nsum(0, ...xs)\n",
          stdout_all=["= 6", "= 6"])
    check("call spread requires list",
          "func f(...xs) { return len(xs); }\nf(...1)\n",
          stderr="spread expects a list, got int")
    check("trait variadic rejected",
          "trait T { func f(self, ...args); }\n",
          stderr="trait methods cannot be variadic")

    many_globals = "".join(f"let v{i} = {i};\n" for i in range(40))
    check("large symbol table lookup", many_globals + "v0 + v39\n",
          stdout="= 39")
    check("large symbol table duplicate", many_globals + "let v0 = 1;\n",
          stderr="symbol repeated")
    check(
        "large symbol table closure",
        many_globals
        + "func make() { let n = 5; func get() { return n + v39; } return get; }\n"
        + "let g = make();\ng()\n",
        stdout="= 44",
    )
    check(
        "large symbol table shadowing",
        many_globals + "let x = 1;\nfunc f() { let x = 2; return x; }\nf()\nx\n",
        stdout_all=["= 2", "= 1"],
    )

    check("call non-function", "let x = 1;\nx(2)\n", stderr="cannot call int")
    check("recursion depth limit",
          "func f(n) { return f(n + 1); }\nf(0)\n",
          stderr="maximum recursion depth")
    check("function without return yields null",
          "func noret() { }\nprint(noret())\n", stdout="null")
    check("null arithmetic", "func noret() { }\nnoret() + 1\n",
          stderr="cannot add null and int")
    check("null truthiness",
          "func noret() { }\nif (noret()) { print(1); } else { print(2); }\n",
          stdout="2")
    check("null as argument",
          "func noret() { }\nfunc add(a,b) { return a+b; }\nadd(noret(), 2)\n",
          stderr="cannot add null and int")
    check("null equality", "func noret() { }\nnoret() == noret()\n",
          stdout="= true")
    check("null literal", "print(null)\n", stdout="null")
    check("null literal equality", "null == null\n", stdout="= true")
    check("null literal inequality", "null == 0\n", stdout="= false")
    check("null literal falsy",
          "if (null) { print(1); } else { print(2); }\n", stdout="2")
    check("null literal arithmetic", "null + 1\n",
          stderr="cannot add null and int")
    check("null is not callable", "null()\n", stderr="cannot call null")
    check("str null", "str(null)\n", stdout="= null")
    check("format null", 'format("{}", null)\n', stdout="= null")

    check("typed parameter ok",
          "func f(x: int) { return x + 1; }\nf(2)\n", stdout="= 3")
    check("typed parameter error",
          'func f(x: int) { return x; }\nf("x")\n',
          stderr="argument 1 (x) expects int, got string")
    check("untyped parameter",
          'func f(x) { return x; }\nf("x")\n', stdout="= x")
    check("multiple typed parameters",
          "func g(a, b: number, c: string) { return b; }\n"
          'g(1, 2.5, "x")\n',
          stdout="= 2.5")
    check("typed number rejects string",
          'func g(b: number) { return b; }\ng("x")\n',
          stderr="expects number, got string")
    check("typed bool",
          "func h(x: bool) { return x; }\nh(true)\nh(false)\n",
          stdout_all=["= true", "= false"])
    check("typed bool rejects int",
          "func h(x: bool) { return x; }\nh(1)\n",
          stderr="expects bool, got int")
    check("typed bool accepts comparison",
          "func h(x: bool) { return x; }\nh(1 == 1)\n",
          stdout="= true")
    check("typed list",
          "func f(x: list) { return len(x); }\nf([1,2])\n",
          stdout="= 2")
    check("typed dict",
          "func f(x: dict) { return len(x); }\nf({1:2})\n",
          stdout="= 1")
    check("typed string",
          'func f(x: string) { return x; }\nf("a")\n', stdout="= a")
    check("typed bytes",
          'func f(x: bytes) { return bytes_len(x); }\n'
          'f(bytes_from_hex("00ff"))\n',
          stdout="= 2")
    check("typed callable",
          "func apply(fn: callable, x: int) { return fn(x); }\n"
          "apply(sqrt, 9)\n",
          stdout="= 3.0")
    check("typed function",
          "func apply(fn: function, x: int) { return fn(x); }\n"
          "let f = func(y) { return y * 2; };\n"
          "apply(f, 3)\n",
          stdout="= 6")
    check("typed builtin",
          "func apply(fn: builtin, x: int) { return fn(x); }\n"
          "apply(sqrt, 16)\n",
          stdout="= 4.0")
    check("typed callable rejects int",
          "func apply(fn: callable) { return fn; }\napply(1)\n",
          stderr="expects callable, got int")
    check("typed function rejects builtin",
          "func apply(fn: function) { return fn; }\napply(sqrt)\n",
          stderr="expects function, got builtin")
    check("typed builtin rejects function",
          "func apply(fn: builtin) { return fn; }\n"
          "let f = func() { return 1; };\napply(f)\n",
          stderr="expects builtin, got function")
    check("typed any",
          "func f(x: any) { return x; }\nf(true)\n", stdout="= true")
    check("typed null",
          "func f(x: null) { return x; }\nprint(f(null))\n",
          stdout="null")
    check("typed null rejects int",
          "func f(x: null) { return x; }\nf(1)\n",
          stderr="expects null, got int")
    check("typed number accepts int and float",
          "func f(x: number) { return x; }\nf(1)\nf(1.5)\n",
          stdout_all=["= 1", "= 1.5"])
    check("typed anonymous function",
          "let f = func(x: int) { return x; };\nf(3)\n", stdout="= 3")
    check("typed closure",
          "func make() { func get(x: int) { return x + 1; } return get; }\n"
          "let f = make();\nf(4)\n",
          stdout="= 5")
    check("typed parameter trailing comma",
          "func f(x: int,) { return x; }\nf(1)\n", stdout="= 1")
    check("typed parameter error prevents body",
          "let hits = 0;\n"
          "func f(x: int) { hits = hits + 1; }\n"
          'f("bad");\nhits\n',
          stdout="= 0",
          stderr="expects int, got string")
    check("unknown type name",
          "func f(x: nope) { return x; }\n",
          stderr="unknown type name nope")

    check("typed variable declaration",
          'let x: string = "hi";\nx\nx = "bye";\nx\n',
          stdout_all=["= hi", "= bye", "= bye"])
    check("typed variable initializer mismatch",
          'let x: int = "bad";\n',
          stderr="variable x expects int, got string")
    check("typed variable reassignment mismatch",
          'let x: int = 1;\nx = "bad";\n',
          stderr="variable x expects int, got string")
    check("typed variable number accepts int and float",
          "let x: number = 1;\nx\nx = 2.5;\nx\n",
          stdout_all=["= 1", "= 2.5", "= 2.5"])
    check("typed variable function",
          "let f: function = func(x) { return x + 1; };\nf(2)\n",
          stdout="= 3")
    check("typed variable builtin",
          "let f: builtin = sqrt;\nf(16)\n", stdout="= 4.0")
    check("typed variable callable",
          "let f: callable = sqrt;\nf(9)\n", stdout="= 3.0")
    check("typed variable requires initializer for function",
          "let f: function;\n",
          stderr="variable f of type function requires an initializer")
    check("typed const declaration",
          "const x: int = 1;\nx\n", stdout_all=["= 1"])
    check("typed const reassignment",
          "const x: int = 1;\nx = 2;\n",
          stderr="cannot assign to constant x")
    check("typed for initializer",
          "for (let i: int = 0; i < 2; i++) { print(i); }\n",
          stdout_all=["0", "1"])
    check("typed parameter reassignment",
          'func f(x: int) { x = "bad"; }\nf(1)\n',
          stderr="variable x expects int, got string")
    check("typed variable defaults",
          "let s: string;\n"
          "let i: int;\n"
          "let f: float;\n"
          "let b: bool;\n"
          "let l: list;\n"
          "let d: dict;\n"
          "let by: bytes;\n"
          "let a: any;\n"
          's == "" && i == 0 && f == 0.0 && !b && len(l) == 0 && '
          "len(d) == 0 && bytes_len(by) == 0 && a == null\n",
          stdout="= true")
    check("typed variable is shallow",
          'let a: list = [0];\na[0] = "x";\na\n', stdout='= [x]')

    check("struct construction and fields",
          "struct Point { x: int; y: int; }\n"
          "let p = Point { x: 3, y: 4 };\n"
          "p.x + p.y\n"
          "p.x = 10;\n"
          "p.x\n"
          "p\n",
          stdout_all=["= 7", "= 10", "= Point { x: 10, y: 4 }"])
    check("struct method in struct",
          "struct Point {\n"
          "  x: int;\n"
          "  y: int;\n"
          "  func length(self) {\n"
          "    return sqrt(self.x * self.x + self.y * self.y);\n"
          "  }\n"
          "}\n"
          "Point { x: 3, y: 4 }.length()\n",
          stdout="= 5.0")
    check("struct method in impl",
          "struct Point { x: int; y: int; }\n"
          "impl Point {\n"
          "  func moved(self, dx: int, dy: int) {\n"
          "    return Point { x: self.x + dx, y: self.y + dy };\n"
          "  }\n"
          "}\n"
          "let p = Point { x: 3, y: 4 };\n"
          "p.moved(1, 2)\n",
          stdout="= Point { x: 4, y: 6 }")
    check("struct method read-only self",
          "struct Counter {\n"
          "  n: int;\n"
          "  func bump(self) { self.n = self.n + 1; }\n"
          "}\n"
          "let c = Counter { n: 1 };\nc.bump();\n",
          stderr="cannot modify self in a read-only method")
    check("struct self reassignment",
          "struct A { func f(self) { self = null; } }\n"
          "A {}.f();\n",
          stderr="cannot assign to constant self")
    check("struct missing field",
          "struct Point { x: int; y: int; }\nPoint { x: 1 }\n",
          stderr="field y of Point is required")
    check("struct unknown field",
          "struct Point { x: int; }\nPoint { x: 1, z: 2 }\n",
          stderr="unknown field z in Point")
    check("struct repeated field",
          "struct Point { x: int; }\nPoint { x: 1, x: 2 }\n",
          stderr="field x repeated in Point")
    check("struct field initializer type",
          'struct Point { x: int; }\nPoint { x: "bad" }\n',
          stderr="field x of Point expects int, got string")
    check("struct field assignment type",
          'struct Point { x: int; }\nlet p = Point { x: 1 };\np.x = "bad";\n',
          stderr="field x of Point expects int, got string")
    check("typed struct variable",
          "struct Point { x: int; }\n"
          "let p: Point = Point { x: 7 };\n"
          "func get_x(p: Point) { return p.x; }\n"
          "get_x(p)\n",
          stdout="= 7")
    check("typed struct variable mismatch",
          "struct Point { x: int; }\nlet p: Point = 1;\n",
          stderr="variable p expects Point, got int")
    check("struct copy-on-write",
          "struct Point { x: int; }\n"
          "let p = Point { x: 1 };\n"
          "let q = p;\n"
          "q.x = 9;\n"
          "p.x\nq.x\n",
          stdout_all=["= 1", "= 9"])
    check("nested struct field",
          "struct Inner { n: int; }\n"
          "struct Outer { inner: Inner; }\n"
          "let o = Outer { inner: Inner { n: 1 } };\n"
          "o.inner.n = 5;\no.inner.n\n",
          stdout="= 5")
    check("struct in list",
          "struct Point { x: int; }\n"
          "let a = [Point { x: 1 }, Point { x: 2 }];\n"
          "a[1].x++;\na[1].x\n",
          stdout="= 3")
    check("struct function field",
          "struct Handler { callback: callable; }\n"
          "let h = Handler { callback: func(x) { return x + 1; } };\n"
          "h.callback(41)\n",
          stdout="= 42")
    check("struct equality",
          "struct Point { x: int; }\n"
          "Point { x: 1 } == Point { x: 1 }\n"
          "Point { x: 1 } == Point { x: 2 }\n",
          stdout_all=["= true", "= false"])
    check("empty struct",
          "struct Empty {}\nlet e = Empty {};\ne\n", stdout="= Empty {}")
    check("bound method value",
          "struct Point {\n"
          "  x: int;\n"
          "  func get(self) { return self.x; }\n"
          "  func add(self, n: int) { return self.x + n; }\n"
          "}\n"
          "let p = Point { x: 3 };\n"
          "let f = p.get;\n"
          "let g = p.add;\n"
          "f()\ng(4)\n",
          stdout_all=["= 3", "= 7"])
    check("bound method snapshot",
          "struct Point { x: int; func get(self) { return self.x; } }\n"
          "let p = Point { x: 1 };\n"
          "let f = p.get;\n"
          "p.x = 5;\n"
          "f()\np.get()\n",
          stdout_all=["= 5", "= 1", "= 5"])

    check("trait implementation",
          "trait Drawable { func draw(self); }\n"
          "struct Point { x: int; }\n"
          "impl Drawable for Point {\n"
          "  func draw(self) { print(\"point\"); }\n"
          "}\n"
          "let p = Point { x: 1 };\n"
          "p.draw()\n"
          "func render(x: Drawable) { x.draw(); }\n"
          "render(p)\n",
          stdout_all=["point", "point"])
    check("trait adapter with existing method",
          "trait Drawable { func draw(self); }\n"
          "struct Point { x: int; }\n"
          "impl Point { func draw(self) { print(\"existing\"); } }\n"
          "impl Drawable for Point;\n"
          "let p = Point { x: 1 };\n"
          "func render(x: Drawable) { x.draw(); }\n"
          "render(p)\n",
          stdout="existing")
    check("trait nominal only",
          "trait Drawable { func draw(self); }\n"
          "struct Point { x: int; }\n"
          "impl Point { func draw(self) { print(\"point\"); } }\n"
          "func render(x: Drawable) { x.draw(); }\n"
          "render(Point { x: 1 });\n",
          stderr="expects Drawable, got Point")
    check("trait typed variable",
          "trait Drawable { func draw(self); }\n"
          "struct Point { x: int; }\n"
          "impl Drawable for Point { func draw(self) { return 1; } }\n"
          "let d: Drawable = Point { x: 1 };\nd.draw()\n",
          stdout="= 1")
    check("trait adapter signature mismatch",
          "trait Drawable { func draw(self, x: int); }\n"
          "struct Point { x: int; }\n"
          "impl Point { func draw(self, x: string) { return x; } }\n"
          "impl Drawable for Point;\n",
          stderr="method draw does not match the trait signature")
    check("trait missing method",
          "trait Drawable { func draw(self); }\n"
          "struct Point { x: int; }\n"
          "impl Drawable for Point { }\n",
          stderr="does not implement method draw required by trait Drawable")
    check("trait extra method",
          "trait Drawable { func draw(self); }\n"
          "struct Point { x: int; }\n"
          "impl Drawable for Point {\n"
          "  func other(self) { return 1; }\n"
          "}\n",
          stderr="method other is not declared by trait Drawable")
    check("duplicate trait implementation",
          "trait Drawable { func draw(self); }\n"
          "struct Point { x: int; }\n"
          "impl Drawable for Point { func draw(self) { return 1; } }\n"
          "impl Drawable for Point { func draw(self) { return 2; } }\n",
          stderr="method repeated draw")
    check("duplicate method",
          "struct Point { x: int; }\n"
          "impl Point { func get(self) { return 1; } }\n"
          "impl Point { func get(self) { return 2; } }\n",
          stderr="method repeated get")
    check("field and method name conflict",
          "struct Point { x: int; func x(self) { return 1; } }\n",
          stderr="field and method have the same name x")
    check("method first parameter self",
          "struct Point { x: int; }\n"
          "impl Point { func get(this) { return 1; } }\n",
          stderr="method first parameter must be self")
    check("unknown struct type",
          "let p = Missing { x: 1 };\n", stderr="unknown struct type Missing")
    check("unknown field type",
          "struct Point { x: Missing; }\n", stderr="unknown type name Missing")
    check("struct switch unsupported",
          "struct Point { x: int; }\n"
          "switch (Point { x: 1 }) { default => print(\"bad\"); }\n",
          stderr="switch does not support Point")

    check("runtime kind_of",
          "kind_of(1)\n"
          "kind_of(1.5)\n"
          "kind_of(true)\n"
          'kind_of("x")\n'
          "kind_of([1])\n"
          "kind_of({})\n"
          "kind_of(func() { return 1; })\n"
          "kind_of(sqrt)\n"
          "kind_of(null)\n",
          stdout_all=["= int", "= float", "= bool", "= string", "= list",
                      "= dict", "= function", "= builtin", "= null"])
    check("runtime type_name",
          "struct Point { x: int; }\n"
          'type_name(1)\ntype_name("x")\ntype_name(Point { x: 1 })\n'
          "type_name(null)\n",
          stdout_all=["= int", "= string", "= Point", "= null"])
    check("runtime type_module for primitive",
          'type_module(1) == ""\n', stdout="= true")
    check("is primitive types",
          "1 is int\n1 is number\n1 is string\n"
          "true is bool\n[1] is list\n{} is dict\n"
          'null is null\n1 is any\n',
          stdout_all=["= true", "= true", "= false", "= true",
                      "= true", "= true", "= true", "= true"])
    check("is struct and trait",
          "trait Drawable { func draw(self); }\n"
          "struct Point { x: int; }\n"
          "struct Other { x: int; }\n"
          "impl Drawable for Point { func draw(self) { return 1; } }\n"
          "let p = Point { x: 1 };\n"
          "let o = Other { x: 1 };\n"
          "p is Point\np is Other\np is Drawable\no is Drawable\n",
          stdout_all=["= true", "= false", "= true", "= false"])
    check("is in heterogeneous loop",
          "struct Point { x: int; }\n"
          'let values = [1, "x", [2], Point { x: 3 }, null];\n'
          "for (let value in values) {\n"
          '  if (kind_of(value) == "struct") {\n'
          '    print("struct " + type_name(value));\n'
          "  } else {\n"
          "    print(kind_of(value));\n"
          "  }\n"
          "}\n",
          stdout_all=["int", "string", "list", "struct Point", "null"])
    check("is unknown type",
          "1 is Missing\n", stderr="unknown type name Missing")

    check("explicit empty return", "func f() { return; }\nprint(f())\n",
          stdout="null")
    check("print returns null", "let x = print(1);\nprint(x)\n",
          stdout_all=["1", "null"])
    check("arity error after argument evaluation",
          "func one(a) { return a; }\none(print(1), 2)\n",
          stdout="1", stderr="expects 1 argument")
    check("non-callable evaluates arguments", "1(print(7))\n",
          stdout="7", stderr="cannot call int")

    check("builtin as value", "let f = sqrt;\nf(9)\n", stdout="= 3.0")
    check("builtin as callback",
          "func apply(f, x) { return f(x); }\napply(sqrt, 16)\n",
          stdout="= 4.0")
    check("builtin in list", "let fs = [sqrt, abs];\nfs[0](25)\n",
          stdout="= 5.0")
    check("builtin equality", "sqrt == sqrt\nsqrt == abs\n",
          stdout_all=["= true", "= false"])
    check("print as value", 'let p = print;\np("ok")\n', stdout="ok")
    check("format as value", 'let f = format;\nf("{} + {}", 1, 2)\n',
          stdout="= 1 + 2")
    check("builtin string form", "str(sqrt)\n", stdout="= <builtin>")
    check("command output", 'import std;\nstd.process.command("echo hi")\n',
          stdout="= hi")
    check("command in format",
          'import std;\nformat("{}", std.process.command("echo hi"))\n',
          stdout="= hi")
    check("command as value",
          'import std;\nlet run = std.process.command;\nrun("echo ok")\n',
          stdout="= ok")
    check("command type error", 'import std;\nstd.process.command(1)\n',
          stderr="command expects a string")
    check("command arity error", 'import std;\nstd.process.command()\n',
          stderr="expects 1 or 2 arguments")
    command_env_test = (
        'import std;\nstd.process.command("echo %FOO%", {"FOO": "bar"})\n'
        if sys.platform == "win32"
        else 'import std;\nstd.process.command("echo $FOO", {"FOO": "bar"})\n'
    )
    check("command environment override", command_env_test, stdout="= bar")
    command_status_env_test = (
        'import std;\nstd.process.command_status("exit %CODE%", '
        '{"CODE": "3"})\n'
        if sys.platform == "win32"
        else 'import std;\nstd.process.command_status("exit $CODE", '
        '{"CODE": "3"})\n'
    )
    check("command_status environment override", command_status_env_test,
          stdout="= 3")
    check("command empty environment",
          'import std;\nstd.process.command("echo hi", dict())\n',
          stdout="= hi")
    check("command env type",
          'import std;\nstd.process.command("echo hi", 1)\n',
          stderr="command env must be a dict")
    check("command env key type",
          'import std;\nstd.process.command("echo hi", {1: "x"})\n',
          stderr="command env keys must be strings")
    check("command env value type",
          'import std;\nstd.process.command("echo hi", {"A": 1})\n',
          stderr="command env values must be strings")
    check("command env invalid name",
          'import std;\nstd.process.command("echo hi", {"A=B": "x"})\n',
          stderr="invalid environment variable name")
    check("command env arity error",
          'import std;\nstd.process.command("echo hi", dict(), 1)\n',
          stderr="expects 1 or 2 arguments")
    check("command_status env type",
          'import std;\nstd.process.command_status("exit 0", 1)\n',
          stderr="command_status env must be a dict")
    check("command_status success",
          'import std;\nstd.process.command_status("exit 0")\n',
          stdout="= 0")
    check("command_status failure",
          'import std;\nstd.process.command_status("exit 3")\n',
          stdout="= 3")
    check("command_status output",
          'import std;\nstd.process.command_status("echo hi")\n',
          stdout_all=["hi", "= 0"])
    check("command_status as value",
          'import std;\nlet run = std.process.command_status;\n'
          'run("exit 0")\n',
          stdout="= 0")
    check("command_status type error",
          'import std;\nstd.process.command_status(1)\n',
          stderr="command_status expects a string")
    check("command_status arity error",
          'import std;\nstd.process.command_status()\n',
          stderr="expects 1 or 2 arguments")
    check("global command removed", 'command("echo hi")\n',
          stderr="symbol not found command")
    check("command is ordinary identifier", "let command = 1;\ncommand\n",
          stdout="= 1")
    check("input reads a line", "input()\nhello\n", stdout="= hello")
    check("input with prompt", 'input("Name: ")\nAlice\n',
          stdout_all=["Name: ", "= Alice"])
    check("input with print", 'print(input("> "))\nworld\n',
          stdout_all=["> ", "world"])
    check("input preserves spaces", "input()\n  hi  \n", stdout="=   hi  ")
    check("input CRLF", "input()\r\nhello\r\n", stdout="= hello")
    check("input backspace", "input()\nhello\x7f\x7f\n", stdout="= hel")
    check("input EOF is null", "input() == null\n", stdout="= true")
    check("input EOF print", "print(input())\n", stdout="null")
    check("input prompt type", "input(1)\n",
          stderr="input prompt must be a string")
    check("input arity", 'input("a", "b")\n',
          stderr="input expects 0 or 1 arguments")
    check("input as value", 'let f = input;\nf("> ")\nvalue\n',
          stdout_all=["> ", "= value"])
    check("read_file type error", "read_file(1)\n",
          stderr="read_file expects a string path")
    check("write_file type error", 'write_file("x", 1)\n',
          stderr="write_file expects a string path and data")
    check("write_file arity error", 'write_file("x")\n',
          stderr="write_file expects 2 arguments")
    check("append_file arity error", 'append_file("x")\n',
          stderr="append_file expects 2 arguments")
    check("file_exists type error", "file_exists(1)\n",
          stderr="file_exists expects a string path")
    check("bytes_from_string display", 'bytes_from_string("abc")\n',
          stdout="= <bytes: 3>")
    check("bytes_len", 'bytes_len(bytes_from_string("abc"))\n', stdout="= 3")
    check("bytes_get", 'bytes_get(bytes_from_string("abc"), 1)\n',
          stdout="= 98")
    check("bytes_get out of range",
          'bytes_get(bytes_from_string("a"), 5)\n',
          stderr="bytes_get index 5 out of range")
    check("bytes_get type", 'bytes_get("x", 0)\n',
          stderr="bytes_get expects bytes")
    check("bytes_slice", 'bytes_hex(bytes_slice('
          'bytes_from_string("abcdef"), 1, 3))\n', stdout="= 626364")
    check("bytes_slice past end",
          'bytes_len(bytes_slice(bytes_from_string("abc"), 10))\n',
          stdout="= 0")
    check("bytes_slice clamps length",
          'bytes_hex(bytes_slice(bytes_from_string("abc"), 1, 100))\n',
          stdout="= 6263")
    check("bytes_concat", 'bytes_hex(bytes_concat('
          'bytes_from_string("ab"), bytes_from_string("cd")))\n',
          stdout="= 61626364")
    check("bytes_hex", 'bytes_hex(bytes_from_hex("00ff10"))\n',
          stdout="= 00ff10")
    check("bytes_from_hex odd", 'bytes_from_hex("0")\n',
          stderr="even number of hex digits")
    check("bytes_from_hex invalid", 'bytes_from_hex("zz")\n',
          stderr="invalid hex digit")
    check("bytes_from_hex type", "bytes_from_hex(1)\n",
          stderr="bytes_from_hex expects a string")
    check("bytes_to_string", 'bytes_to_string(bytes_from_hex("68656c6c6f"))\n',
          stdout="= hello")
    check("bytes_to_string NUL", 'bytes_to_string(bytes_from_hex("00"))\n',
          stderr="bytes contain NUL byte")
    check("bytes_to_string invalid utf8",
          'bytes_to_string(bytes_from_hex("ff"))\n',
          stderr="not valid UTF-8")
    check("bytes_to_string overlong utf8",
          'bytes_to_string(bytes_from_hex("c080"))\n',
          stderr="not valid UTF-8")
    check("bytes_to_string surrogate utf8",
          'bytes_to_string(bytes_from_hex("eda080"))\n',
          stderr="not valid UTF-8")
    check("bytes_to_string utf8 roundtrip",
          'bytes_to_string(bytes_from_string("你好"))\n',
          stdout="= 你好")
    check("bytes_to_string emoji roundtrip",
          'bytes_to_string(bytes_from_string("😀"))\n',
          stdout="= 😀")
    check("bytes_to_int little",
          'bytes_to_int(bytes_from_hex("78563412"), 4, "little")\n',
          stdout="= 305419896")
    check("bytes_to_int big",
          'bytes_to_int(bytes_from_hex("78563412"), 4, "big")\n',
          stdout="= 2018915346")
    check("bytes_to_int signed 16",
          'bytes_to_int(bytes_from_hex("ffff"), 2, "little", 0, 1)\n',
          stdout="= -1")
    check("bytes_to_int unsigned 16",
          'bytes_to_int(bytes_from_hex("ffff"), 2, "little", 0, 0)\n',
          stdout="= 65535")
    check("bytes_to_int signed 8",
          'bytes_to_int(bytes_from_hex("80"), 1, "big", 0, 1)\n',
          stdout="= -128")
    check("bytes_to_int offset",
          'bytes_to_int(bytes_from_hex("00ff80"), 2, "big", 1)\n',
          stdout="= 65408")
    check("bytes_to_int signed 64",
          'bytes_to_int(bytes_from_hex("ffffffffffffffff"), 8, "little", 0, 1)\n',
          stdout="= -1")
    check("bytes_to_int unsigned 64 overflow",
          'bytes_to_int(bytes_from_hex("ffffffffffffffff"), 8, "little")\n',
          stderr="unsigned 64-bit value is too large")
    check("bytes_to_int truncated",
          'bytes_to_int(bytes_from_hex("0102"), 4, "little")\n',
          stderr="need 4 bytes at offset 0, got 2")
    check("bytes_to_int size range",
          'bytes_to_int(bytes_from_hex("01"), 0, "little")\n',
          stderr="size must be between 1 and 8")
    check("bytes_to_int byteorder",
          'bytes_to_int(bytes_from_hex("01"), 1, "middle")\n',
          stderr='byteorder must be "little" or "big"')
    check("bytes_to_int negative offset",
          'bytes_to_int(bytes_from_hex("01"), 1, "little", -1)\n',
          stderr="offset must be a non-negative integer")
    check("bytes_to_int type", 'bytes_to_int("x", 1, "little")\n',
          stderr="bytes_to_int expects bytes")
    check("bytes_to_int arity",
          'bytes_to_int(bytes_from_hex("01"))\n',
          stderr="bytes_to_int expects 3 to 5 arguments")
    check("int_to_bytes little",
          'bytes_hex(int_to_bytes(305419896, 4, "little"))\n',
          stdout="= 78563412")
    check("int_to_bytes big",
          'bytes_hex(int_to_bytes(305419896, 4, "big"))\n',
          stdout="= 12345678")
    check("int_to_bytes negative",
          'bytes_hex(int_to_bytes(-1, 2, "little"))\n', stdout="= ffff")
    check("int_to_bytes negative big",
          'bytes_hex(int_to_bytes(-2, 4, "big"))\n', stdout="= fffffffe")
    check("int_to_bytes unsigned 32",
          'bytes_hex(int_to_bytes(4294967295, 4, "little"))\n',
          stdout="= ffffffff")
    check("int_to_bytes range high",
          'int_to_bytes(256, 1, "little")\n',
          stderr="does not fit in 1 bytes")
    check("int_to_bytes range low",
          'int_to_bytes(-129, 1, "little")\n',
          stderr="does not fit in 1 bytes")
    check("int_to_bytes size range",
          'int_to_bytes(1, 9, "little")\n',
          stderr="size must be between 1 and 8")
    check("int_to_bytes byteorder",
          'int_to_bytes(1, 4, "middle")\n',
          stderr='byteorder must be "little" or "big"')
    check("int_to_bytes type", 'int_to_bytes("x", 4, "little")\n',
          stderr="int_to_bytes expects an integer value")
    check("int_to_bytes arity", 'int_to_bytes(1, 4)\n',
          stderr="int_to_bytes expects 3 arguments")
    check("bytes/int roundtrip",
          'bytes_to_int(int_to_bytes(-123456789, 8, "big"), '
          '8, "big", 0, 1)\n',
          stdout="= -123456789")
    check("bytes index read", 'bytes_from_string("hello")[1]\n',
          stdout="= 101")
    check("bytes index out of range", 'bytes_from_string("a")[5]\n',
          stderr="bytes index 5 out of range")
    check("bytes index assignment",
          'let b = bytes_from_string("abc");\n'
          'b[1] = 255;\n'
          'bytes_hex(b)\n',
          stdout="= 61ff63")
    check("bytes index assignment range",
          'let b = bytes_from_string("abc");\n'
          'b[0] = 256;\n',
          stderr="bytes value 256 out of range")
    check("bytes equality",
          'bytes_from_string("ab") == bytes_from_hex("6162")\n',
          stdout="= true")
    check("bytes arity", "bytes_len()\n",
          stderr="built-in function expects 1 argument")
    check("bytes_slice arity", "bytes_slice(bytes_from_string(\"a\"))\n",
          stderr="bytes_slice expects 2 or 3 arguments")
    check("read_bytes type", "read_bytes(1)\n",
          stderr="read_bytes expects a string path")
    check("write_bytes type", 'write_bytes("x", "text")\n',
          stderr="write_bytes expects a string path and bytes")
    check("bytes hex roundtrip type", "bytes_hex(1)\n",
          stderr="bytes_hex expects bytes")
