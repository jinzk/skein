#!/usr/bin/env python3
"""Run worker-pool and thread workloads under LeakSanitizer."""
import os
import subprocess
import sys
import tempfile

BIN = sys.argv[1]

WORKER = """
func handle(payload) {
    return {"v": payload["n"] + 1};
}
func main(ctx) {
    return {"v": ctx.payload["n"] + 2};
}
"""


def main():
    with tempfile.TemporaryDirectory() as tmp:
        worker = os.path.join(tmp, "worker.skein")
        with open(worker, "w", encoding="utf-8") as handle:
            handle.write(WORKER)
        source = (
            "import std;\n"
            f'let pool = std.thread.pool(2, "{worker.replace(chr(92), chr(92) * 2)}");\n'
            'let r1 = pool.submit({"n": 1}).wait();\n'
            'let r2 = pool.submit({"n": 2}).wait();\n'
            "pool.close();\n"
            "pool.join();\n"
            f'let msg = std.thread.Message.from({{"n": 3}});\n'
            f'let t = std.thread.spawn('
            f'"{worker.replace(chr(92), chr(92) * 2)}", msg);\n'
            "let joined = t.join();\n"
            'let src = "func main(ctx) {\\n'
            '    return {\\"v\\": ctx.payload[\\"n\\"] + 2};\\n'
            '}\\n";\n'
            f'let t2 = std.thread.spawn_source(src, '
            f'std.thread.Message.from({{"n": 3}}));\n'
            "let joined2 = t2.join();\n"
            "thread func proto_worker(ctx) {\n"
            '    return {"v": ctx.payload["n"] + 3};\n'
            "}\n"
            'let t3 = std.thread.spawn(proto_worker, '
            'std.thread.Message.from({"n": 3}));\n'
            "let joined3 = t3.join();\n"
            "func make_worker(n) {\n"
            "    thread func inner(ctx) {\n"
            '        return {"v": ctx.payload["n"] + n};\n'
            "    }\n"
            "    return inner;\n"
            "}\n"
            "let captured = make_worker(4);\n"
            "let t4 = std.thread.spawn(captured, "
            'std.thread.Message.from({"n": 3}));\n'
            "let joined4 = t4.join();\n"
            "func make_data() {\n"
            "    let data = [1, 2, 3];\n"
            "    thread func inner(ctx) {\n"
            "        return data;\n"
            "    }\n"
            "    return inner;\n"
            "}\n"
            "let captured_data = make_data();\n"
            "let t5 = std.thread.spawn(captured_data, "
            'std.thread.Message.from({"n": 0}));\n'
            "let joined5 = t5.join();\n"
            "func make_adder(n) {\n"
            "    return func(x) { return x + n; };\n"
            "}\n"
            "let adder = make_adder(3);\n"
            "func helper(x) { return x * 2; }\n"
            "func make_transfer(f) {\n"
            "    thread func inner(ctx) {\n"
            '        return {"v": f(ctx.payload["n"]) + '
            'helper(ctx.payload["n"])};\n'
            "    }\n"
            "    return inner;\n"
            "}\n"
            "let transfer = make_transfer(adder);\n"
            "let t6 = std.thread.spawn(transfer, "
            'std.thread.Message.from({"n": 3}));\n'
            "let joined6 = t6.join();\n"
            "let encoded_blob = std.thread.encode(transfer);\n"
            "let t7 = std.thread.spawn_encoded(encoded_blob, "
            'std.thread.Message.from({"n": 3}));\n'
            "let joined7 = t7.join();\n"
            "let sval = std.set.from_list([1, 2]);\n"
            "let dval = std.deque.from_list([3, 4]);\n"
            "let lval = std.lru.new(1);\n"
            'let lr = std.lru.put(lval, "x", 9);\n'
            'lval = lr["lru"];\n'
            "func make_encoded_containers(vs, vd, vl) {\n"
            "    thread func inner(ctx) {\n"
            '        let n = ctx.payload["n"];\n'
            "        let m = match n { case 1 => 5; default => 0; };\n"
            '        let got = std.lru.get(vl, "x");\n'
            '        return {"s": std.set.values(vs), '
            '"d": std.deque.to_list(vd), "l": got["value"], "m": m};\n'
            "    }\n"
            "    return inner;\n"
            "}\n"
            "let encoded_containers = std.thread.encode("
            "make_encoded_containers(sval, dval, lval));\n"
            "let t8 = std.thread.spawn_encoded(encoded_containers, "
            'std.thread.Message.from({"n": 1}));\n'
            "let joined8 = t8.join();\n"
            'println("pool=" + str(r1["value"]["v"]) + ":" + '
            'str(r2["value"]["v"]) + " thread=" + '
            'str(joined["value"]["v"]) + " src=" + '
            'str(joined2["value"]["v"]) + " proto=" + '
            'str(joined3["value"]["v"]) + " closure=" + '
            'str(joined4["value"]["v"]) + " data=" + '
            'str(joined5["value"][2]) + " transfer=" + '
            'str(joined6["value"]["v"]) + " encoded=" + '
            'str(joined7["value"]["v"]) + " enc=" + '
            'str(joined8["value"]["s"][0]) + ":" + '
            'str(joined8["value"]["d"][0]) + ":" + '
            'str(joined8["value"]["l"]) + ":" + '
            'str(joined8["value"]["m"]));\n'
        )
        env = os.environ.copy()
        env.setdefault("ASAN_OPTIONS", "detect_leaks=1")
        proc = subprocess.run(
            [BIN],
            input=source,
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=180,
            env=env,
        )
        if proc.returncode != 0:
            sys.stderr.write(proc.stderr)
            return 1
        expected = ("pool=2:3 thread=5 src=5 proto=6 closure=7 data=3 "
                    "transfer=12 encoded=12 enc=1:3:9:5")
        if expected not in proc.stdout:
            sys.stderr.write(proc.stdout + proc.stderr)
            return 1

        pkg_dir = os.path.join(tmp, "leakpkg")
        os.makedirs(pkg_dir)
        with open(os.path.join(pkg_dir, "leakpkg.skein"), "w",
                  encoding="utf-8") as handle:
            handle.write(
                "package leakpkg {\n"
                "  let base = 10;\n"
                "  func helper(x) { return x + base; }\n"
                "  export let offset = 3;\n"
                "  export func calc(x) { return helper(x) + offset; }\n"
                "}\n"
            )
        main_path = os.path.join(tmp, "import_main.skein")
        with open(main_path, "w", encoding="utf-8") as handle:
            handle.write(
                "import std;\n"
                "import leakpkg;\n"
                "func make_worker() {\n"
                "    thread func worker(ctx) {\n"
                '        return {"v": leakpkg.calc(ctx.payload["n"])};\n'
                "    }\n"
                "    return worker;\n"
                "}\n"
                "let w = make_worker();\n"
                "let t = std.thread.spawn(w, "
                'std.thread.Message.from({"n": 4}));\n'
                "let r = t.join();\n"
                'print("import=" + str(r["value"]["v"]));\n'
            )
        proc = subprocess.run(
            [BIN, main_path],
            cwd=tmp,
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=180,
            env=env,
        )
        if proc.returncode != 0:
            sys.stderr.write(proc.stderr)
            return 1
        if "import=17" not in proc.stdout:
            sys.stderr.write(proc.stdout + proc.stderr)
            return 1
        return 0


if __name__ == "__main__":
    sys.exit(main())
