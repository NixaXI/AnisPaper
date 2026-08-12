#!/usr/bin/env python3
"""Regression test for numeric strings in Workshop Scene JSON.

The vendored parser is header-templated, so compile a small consumer against
the exact source header.  This checks accepted numeric strings and ensures
partial/non-finite/non-numeric strings still fail closed.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile


def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[1]
    source = r'''
#include "WallpaperEngine/Data/JSON.h"

#include <cmath>
#include <stdexcept>

using WallpaperEngine::Data::JSON::JSON;

int main() {
    const JSON input = JSON::parse(R"({
        "integer": "42", "negative": "-7", "decimal": "12.5",
        "badSuffix": "12.5px", "badEmpty": "", "nonFinite": "nan"
    })");
    if (input.optional<int>("integer", -1) != 42) return 1;
    if (input.optional<int>("negative", 0) != -7) return 2;
    if (std::fabs(input.optional<float>("decimal", 0.0f) - 12.5f) > 0.0001f) return 3;
    for (const char* key : {"badSuffix", "badEmpty", "nonFinite"}) {
        bool rejected = false;
        try { (void)input.optional<float>(key, 0.0f); }
        catch (const std::invalid_argument&) { rejected = true; }
        if (!rejected) return 4;
    }
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="anispaper-scene-json-") as temp:
        directory = pathlib.Path(temp)
        program = directory / "scene_json_numeric_string_test.cpp"
        binary = directory / "scene_json_numeric_string_test"
        program.write_text(source, encoding="utf-8")
        compile_result = subprocess.run(
            ["g++", "-std=c++20", "-Wall", "-Wextra", "-Wpedantic",
             "-I", str(root / "third_party/linux-wallpaperengine/src"),
             "-I", str(root / "third_party/linux-wallpaperengine/src/External/json/include"),
             str(program), "-o", str(binary)],
            text=True, capture_output=True, check=False)
        if compile_result.returncode:
            print(compile_result.stdout + compile_result.stderr, file=sys.stderr)
            return compile_result.returncode
        run_result = subprocess.run([str(binary)], check=False)
        if run_result.returncode == 0:
            print("scene_json_numeric_string_test: PASS")
        return run_result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
