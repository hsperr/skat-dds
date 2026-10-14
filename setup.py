"""Builds src/skatdds.cpp into the package as libskatdds.{dylib,so} (plain ctypes, no Python
extension module). Set SKATDDS_PORTABLE=1 to build without -march=native / -mcpu=native."""
import os
import subprocess
import sys

from setuptools import setup
from setuptools.command.build_py import build_py
from setuptools.dist import Distribution

HERE = os.path.dirname(os.path.abspath(__file__))


def _cpu_flag(cxx):
    if os.environ.get("SKATDDS_PORTABLE") == "1":
        return []
    for flag in ("-march=native", "-mcpu=native"):
        r = subprocess.run([cxx, flag, "-x", "c++", "-fsyntax-only", os.devnull],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if r.returncode == 0:
            return [flag]
    return []


class BuildPyWithLib(build_py):
    def run(self):
        super().run()
        ext = "dylib" if sys.platform == "darwin" else "so"
        out = os.path.join(self.build_lib, "skatdds", "libskatdds." + ext)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        cxx = os.environ.get("CXX", "c++")
        cmd = [cxx, "-O3", *_cpu_flag(cxx), "-std=c++17", "-fPIC", "-shared",
               "-I", os.path.join(HERE, "include"), "-o", out,
               os.path.join(HERE, "src", "skatdds.cpp"), "-lpthread"]
        print(" ".join(cmd))
        subprocess.check_call(cmd)


class BinaryDistribution(Distribution):
    def has_ext_modules(self):
        return True


setup(cmdclass={"build_py": BuildPyWithLib}, distclass=BinaryDistribution)
