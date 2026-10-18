#!/usr/bin/env python3
"""Собирает C++ движок и тесты установленным компилятором без зависимости от CMake."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import pyarrow

ROOT = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser()
parser.add_argument("--sanitize", action="store_true")
args = parser.parse_args()
# Разные каталоги не позволяют смешать объектные файлы release и сборки с санитайзерами.
out = ROOT / "build" / ("sanitize" if args.sanitize else "release")
out.mkdir(parents=True, exist_ok=True)
# Wheel предоставляет нативные библиотеки; ссылки дают стандартные имена для -larrow/-lparquet.
pyarrow.create_library_symlinks()
lib = Path(pyarrow.get_library_dirs()[0])
# CXX может включать обёртку компилятора; аргументы запускаются напрямую, без shell.
compiler = shlex.split(os.environ.get("CXX", "c++"))
flags = ["-std=c++17", "-Wall", "-Wextra", "-Wpedantic", "-I" + str(ROOT / "include")]
flags += ["-isystem", pyarrow.get_include()]
flags += ["-I" + str(ROOT / "third_party/snowball")]
flags += ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else ["-O3", "-DNDEBUG"]
# macOS предоставляет системную ICU; на Linux флаги узнаём через pkg-config.
if sys.platform == "darwin":
    icu = ["-licucore"]
else:
    flags += shlex.split(subprocess.check_output(["pkg-config", "--cflags", "icu-uc"], text=True))
    icu = shlex.split(subprocess.check_output(["pkg-config", "--libs", "icu-uc"], text=True))
core = ["common", "codec", "tokenizer", "index", "query", "ranking"]
# Официальные сгенерированные C-исходники Snowball включены в проект и компилируются локально.
stemmer_objects = []
for name in ["runtime/api", "runtime/utilities", "src_c/stem_UTF_8_english"]:
    obj = out / (Path(name).name + ".o")
    cflags = [flag for flag in flags if flag not in {"-std=c++17", "-Wpedantic"}]
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + ["-std=c99"] + cflags +
                   ["-c", str(ROOT / "third_party/snowball" / (name + ".c")), "-o", str(obj)], check=True)
    stemmer_objects.append(str(obj))
# Общие модули компилируются один раз и затем используются в обоих бинарных файлах.
for name in core + ["parquet", "main", "tests"]:
    source = ROOT / ("tests/tests.cpp" if name == "tests" else "src/" + name + ".cpp")
    subprocess.run(compiler + flags + ["-c", str(source), "-o", str(out / (name + ".o"))], check=True)
objects = [str(out / (name + ".o")) for name in core] + stemmer_objects
icu += ["-lz"]
subprocess.run(compiler + flags + objects + [str(out / "tests.o")] + icu + ["-o", str(out / "l0_tests")], check=True)
# rpath позволяет загрузчику найти библиотеки Arrow из локального Python-окружения.
subprocess.run(compiler + flags + objects + [str(out / "parquet.o"), str(out / "main.o"), "-L" + str(lib), "-Wl,-rpath," + str(lib), "-lparquet", "-larrow"] + icu + ["-o", str(out / "l0")], check=True)
print(out / "l0")
