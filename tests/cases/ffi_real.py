"""End-to-end FFI checks against real system C libraries."""

import ctypes.util
import json
import os
import subprocess
import tempfile

from harness import *  # noqa: F401,F403


def _ffi_available():
    return "= dict" in run("import std;\nkind_of(std.ffi)\n").stdout


def _write_manifest(directory, name, manifest):
    path = os.path.join(directory, name)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(manifest, handle)
    return path


def _mysql_bindgen_case(directory):
    headers = (
        "/usr/include/mysql/mysql.h",
        "/usr/local/include/mysql/mysql.h",
        "/usr/include/mariadb/mysql.h",
    )
    header = next((path for path in headers if os.path.exists(path)), None)
    if not header:
        return
    output = os.path.join(directory, "mysql.ffi.json")
    proc = subprocess.run(
        [BIN, "--ffi-gen", "--header", header, "--allow-opaque-types",
         "--symbols", "mysql_*", "-o", output],
        text=True, capture_output=True, timeout=60)
    assert proc.returncode == 0, (proc.stdout, proc.stderr)
    with open(output, encoding="utf-8") as handle:
        manifest = json.load(handle)
    for name in ("mysql_init", "mysql_real_connect", "mysql_query",
                 "mysql_store_result", "mysql_fetch_row", "mysql_close"):
        assert name in manifest["functions"], name


def _sqlite_cases(directory):
    library = ctypes.util.find_library("sqlite3")
    if not library:
        return
    manifest = _write_manifest(directory, "sqlite.ffi.json", {
        "ffi": 1,
        "types": {
            "sqlite3": {"kind": "opaque"},
            "sqlite3Ptr": {"kind": "pointer", "to": "sqlite3"},
            "sqlite3PtrPtr": {"kind": "pointer", "to": "sqlite3Ptr"},
            "ExecCb": {
                "kind": "callback",
                "result": "i32",
                "args": ["ptr", "i32", "ptr", "ptr"],
            },
        },
        "functions": {
            "sqlite3_libversion": {"result": "cstring", "args": []},
            "sqlite3_open": {
                "result": "i32",
                "args": ["cstring", "sqlite3PtrPtr"],
            },
            "sqlite3_close": {
                "result": "i32",
                "args": ["sqlite3Ptr"],
                "consumes": [0],
            },
            "sqlite3_exec": {
                "result": "i32",
                "args": ["sqlite3Ptr", "cstring", "ExecCb", "ptr", "ptr"],
            },
            "sqlite3_errmsg": {
                "result": "cstring",
                "args": ["sqlite3Ptr"],
            },
            "sqlite3_mprintf": {
                "result": "ptr",
                "args": ["cstring"],
                "variadic": True,
                "returns_owned": {"free": "sqlite3_free"},
            },
        },
    })
    check(
        "ffi sqlite3",
        "import std;\n"
        f"let lib = std.ffi.open({skein_string(library)}, "
        f"{skein_string(manifest)});\n"
        'let sqlite3 = std.ffi.type(lib, "sqlite3");\n'
        'let sqlite3Ptr = std.ffi.type(lib, "sqlite3Ptr");\n'
        'let ExecCb = std.ffi.type(lib, "ExecCb");\n'
        "let out = std.ffi.alloc(std.ffi.pointer(sqlite3));\n"
        'let rc = std.ffi.call(lib, "sqlite3_open", [":memory:", out]);\n'
        "let db = std.ffi.deref(out);\n"
        "let state = {\"rows\": 0};\n"
        "let cb = std.ffi.callback(ExecCb, func(user, n, values, names) {\n"
        "  state[\"rows\"] = state[\"rows\"] + 1;\n"
        "  return 0;\n"
        "});\n"
        'std.ffi.call(lib, "sqlite3_exec", '
        '[db, "CREATE TABLE t(x INTEGER);", null, null, null]);\n'
        'std.ffi.call(lib, "sqlite3_exec", '
        '[db, "INSERT INTO t VALUES (1), (2);", null, null, null]);\n'
        'std.ffi.call(lib, "sqlite3_exec", '
        '[db, "SELECT * FROM t;", cb, null, null]);\n'
        "state[\"rows\"]\n"
        'let bad = std.ffi.call(lib, "sqlite3_exec", '
        '[db, "NOT SQL;", null, null, null]);\n'
        "bad != 0\n"
        'std.string.len(std.ffi.call(lib, "sqlite3_errmsg", [db])) > 0\n'
        'let raw = std.ffi.call_variadic(lib, "sqlite3_mprintf", '
        '["%d-%s"], ["i32", "cstring"], [7, "ok"]);\n'
        'std.ffi.string(raw)\n'
        "raw = null;\n"
        'std.string.len(std.ffi.call(lib, "sqlite3_libversion", [])) > 0\n'
        'std.ffi.call(lib, "sqlite3_close", [db]);\n'
        "db = null;\n"
        "out = null;\n"
        "std.ffi.close(lib);\n",
        stdout_all=["= 0", "= 2", "= true", "= true", "= 7-ok",
                    "= true"],
    )


def _zlib_cases(directory):
    library = ctypes.util.find_library("z")
    if not library:
        return
    manifest = _write_manifest(directory, "zlib.ffi.json", {
        "ffi": 1,
        "types": {
            "SizePtr": {"kind": "pointer", "to": "usize"},
        },
        "functions": {
            "zlibVersion": {"result": "cstring", "args": []},
            "compressBound": {"result": "usize", "args": ["usize"]},
            "compress2": {
                "result": "i32",
                "args": ["buffer", "SizePtr", "bytes", "usize", "i32"],
            },
            "uncompress": {
                "result": "i32",
                "args": ["buffer", "SizePtr", "bytes", "usize"],
            },
        },
    })
    check(
        "ffi zlib",
        "import std;\n"
        f"let lib = std.ffi.open({skein_string(library)}, "
        f"{skein_string(manifest)});\n"
        'let source = bytes_from_string("hello hello hello");\n'
        "let source_len = std.bytes.len(source);\n"
        'let bound = std.ffi.call(lib, "compressBound", [source_len]);\n'
        "let dest = std.ffi.buffer(bound);\n"
        'let dest_len = std.ffi.alloc("usize");\n'
        "std.ffi.set(dest_len, 0, bound);\n"
        'let compressed_rc = std.ffi.call(lib, "compress2", '
        "[dest, dest_len, source, source_len, 9]);\n"
        "let compressed_len = std.ffi.deref(dest_len);\n"
        "let compressed = std.ffi.buffer_bytes(dest, compressed_len);\n"
        "let out = std.ffi.buffer(source_len);\n"
        'let out_len = std.ffi.alloc("usize");\n'
        "std.ffi.set(out_len, 0, source_len);\n"
        'let unpack_rc = std.ffi.call(lib, "uncompress", '
        "[out, out_len, compressed, compressed_len]);\n"
        "compressed_rc == 0 && unpack_rc == 0\n"
        "bytes_to_string(std.ffi.buffer_bytes(out, source_len))\n"
        'std.string.len(std.ffi.call(lib, "zlibVersion", [])) > 0\n'
        "std.ffi.close(lib);\n",
        stdout_all=["= true", "= hello hello hello", "= true"],
    )


def run_cases():
    if not _ffi_available():
        return
    with tempfile.TemporaryDirectory() as directory:
        _sqlite_cases(directory)
        _zlib_cases(directory)
        _mysql_bindgen_case(directory)
