#!/usr/bin/env python3
"""Build a pinned, shared Qt 5.15 toolchain from verified official sources.

Run on macOS (Xcode selected by DEVELOPER_DIR) or in an MSVC developer shell.
The cache contains no certificates or app binaries. Linux uses distro Qt instead.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import zipfile

VERSION = "5.15.19"
SOURCES = {
    "qtbase": "51e91c73abacab81e64efd01bf95794e79c0e605ad80947a024769f0dd620a32",
    "qtsvg": "cabc95fc12fa84c92e95f08660742a0ce297e6db3deadbefbe7fbec4769b8c13",
    "qttools": "2694af886130d63b8a42bcd138f4035fd8108273ebd16781f87300da352f1aa6",
}
JOM_SHA256 = "d533c1ef49214229681e90196ed2094691e8c4a0a0bef0b2c901debcb562682b"


def run(command, cwd=None, env=None):
    print("+", subprocess.list2cmdline(list(map(str, command))), flush=True)
    subprocess.run(list(map(str, command)), cwd=cwd, env=env, check=True)


def download(url, destination, expected):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.exists():
        partial = destination.with_suffix(destination.suffix + ".partial")
        run(["curl", "--fail", "--location", "--retry", "4", "--connect-timeout", "30",
             "--max-time", "900", "--output", partial, url])
        partial.replace(destination)
    actual = hashlib.sha256(destination.read_bytes()).hexdigest()
    if actual != expected:
        raise RuntimeError(f"SHA256 mismatch: {destination.name}; remove the file before retrying")


def extract(source, destination):
    """Reject traversal and out-of-tree links, including on Windows."""
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(source, "r:xz") as archive:
        archive.extractall(destination, filter="data")


def configure_args(prefix, system, arch, deployment_target):
    args = ["-prefix", str(prefix), "-opensource", "-confirm-license", "-release", "-shared",
            "-nomake", "tests", "-nomake", "examples", "-no-opengl", "-no-dbus", "-no-icu",
            "-no-openssl", "-no-zstd", "-qt-zlib", "-qt-libpng",
            "-qt-libjpeg", "-qt-pcre", "-qt-harfbuzz"]
    # Avoid discovering database client libraries from the CI host.
    args += ["-no-sql-" + driver for driver in
             ["db2", "ibase", "mysql", "oci", "odbc", "psql", "sqlite", "sqlite2", "tds"]]
    if system == "Darwin":
        args += ["-securetransport", "-no-pkg-config", f"QMAKE_APPLE_DEVICE_ARCHS={arch}",
                 f"QMAKE_MACOSX_DEPLOYMENT_TARGET={deployment_target}"]
    else:
        args += ["-platform", "win32-msvc", "-schannel"]
    return args


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--arch", choices=["x86_64", "arm64", "x86"], required=True)
    parser.add_argument("--deployment-target", default="")
    parser.add_argument("--host-bindir", type=Path, help="Matching native Qt tools for a macOS cross-build")
    parser.add_argument("--parallel", type=int, default=min(os.cpu_count() or 2, 4))
    parser.add_argument("--download-only", action="store_true", help="Verify and extract sources without compiling")
    args = parser.parse_args()
    system = platform.system()
    if system not in {"Darwin", "Windows"}:
        parser.error("This toolchain builder supports macOS and Windows; use distro Qt on Linux")
    if args.parallel < 1:
        parser.error("--parallel must be positive")
    if system == "Darwin" and (args.arch == "x86" or not args.deployment_target):
        parser.error("macOS requires x86_64/arm64 and an explicit --deployment-target")
    if system == "Windows" and args.arch == "arm64":
        parser.error("Windows ARM64 is not part of this build matrix")
    prefix, work = args.prefix.resolve(), args.work.resolve()
    host = args.host_bindir.resolve() if args.host_bindir else None
    if host:
        if system != "Darwin":
            parser.error("--host-bindir currently supports macOS only")
        version = subprocess.check_output([str(host / 'qmake'), '-query', 'QT_VERSION'], text=True).strip()
        if version != VERSION:
            parser.error("Host tools must match Qt " + VERSION)
    if " " in str(work) or " " in str(prefix):
        parser.error("Qt 5 source builds require work/prefix paths without spaces")
    marker = prefix / "client-toolchain.json"
    expected = {"qt": VERSION, "system": system, "arch": args.arch,
                "deployment_target": args.deployment_target, "host_bindir": str(host) if host else None,
                "recipe": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    suffix = ".exe" if system == "Windows" else ""
    deploy_name = "macdeployqt" if system == "Darwin" else "windeployqt.exe"
    required = [(host or prefix / "bin") / ("qmake" + suffix), (host or prefix / "bin") / deploy_name,
                prefix / "lib/cmake/Qt5Svg/Qt5SvgConfig.cmake",
                prefix / "lib/cmake/Qt5Test/Qt5TestConfig.cmake"]
    if marker.is_file() and json.loads(marker.read_text()) == expected and all(p.is_file() for p in required):
        print(f"Using verified Qt {VERSION} toolchain cache at {prefix}")
        return
    if prefix.exists() and any(prefix.iterdir()):
        parser.error("Prefix is nonempty without a matching complete toolchain; use a fresh prefix")
    sources = {}
    for module, sha256 in SOURCES.items():
        name = f"{module}-everywhere-opensource-src-{VERSION}.tar.xz"
        archive = work / "downloads" / name
        download(f"https://download.qt.io/archive/qt/5.15/{VERSION}/submodules/{name}", archive, sha256)
        target = work / "sources" / f"{module}-everywhere-src-{VERSION}"
        if not target.is_dir():
            extract(archive, work / "sources")
        sources[module] = target
    if args.download_only:
        print("All official source archive SHA256 values verified and sources extracted")
        return
    env = os.environ.copy()
    env["PATH"] = str(prefix / "bin") + os.pathsep + env["PATH"]
    if system == "Windows":
        target_arch = env.get("VSCMD_ARG_TGT_ARCH", "")
        if target_arch != ("x86" if args.arch == "x86" else "x64"):
            parser.error("Select the matching MSVC developer environment before building Qt")
        archive = work / "downloads/jom_1_1_4.zip"
        download("https://download.qt.io/official_releases/jom/jom_1_1_4.zip", archive, JOM_SHA256)
        jom_dir = work / "jom"
        jom_dir.mkdir(exist_ok=True)
        with zipfile.ZipFile(archive) as zipped:
            zipped.extractall(jom_dir)
        make = [str(jom_dir / "jom.exe"), "-j", str(args.parallel)]
    else:
        make = ["make", "-j", str(args.parallel)]
        env["MACOSX_DEPLOYMENT_TARGET"] = args.deployment_target
    for tool in ["perl", "cl" if system == "Windows" else "clang++"]:
        if not shutil.which(tool):
            parser.error(f"Required source-build tool is missing: {tool}")
    base_build = work / "build-base"
    base_build.mkdir(parents=True, exist_ok=True)
    configure = sources["qtbase"] / ("configure.bat" if system == "Windows" else "configure")
    command = [str(configure)] + configure_args(prefix, system, args.arch, args.deployment_target)
    if host:
        command += ["-external-hostbindir", str(host)]
    # configure.bat is part of the pinned source, and every argument is CLI-derived or constant.
    if system == "Windows":
        command = ["cmd.exe", "/d", "/c"] + command
    run(command, cwd=base_build, env=env)
    run(make, cwd=base_build, env=env)
    run(make + ["install"], cwd=base_build, env=env)
    for module in ["qtsvg", "qttools"]:
        if host and module == "qttools":
            continue # Deployment tools run on the host, not the target CPU.
        build = work / ("build-" + module)
        build.mkdir(exist_ok=True)
        command = ([host / "qmake", "-qtconf", base_build / "bin/qt.conf"] if host
                   else [prefix / "bin" / ("qmake" + suffix)]) + [sources[module] / (module + ".pro")]
        if module == "qttools":
            # Keep deployment tools; the application uses no Designer, QML or documentation tools.
            disabled = ["assistant", "designer", "distancefieldgenerator", "kmap2qmap", "linguist",
                        "makeqpf", "pixeltool", "qdbus", "qev", "qtattributionsscanner", "qtdiag",
                        "qtpaths", "qtplugininfo", "qdoc"]
            command += ["--"] + ["-no-feature-" + name for name in disabled]
        run(command, cwd=build, env=env)
        run(make, cwd=build, env=env)
        run(make + ["install"], cwd=build, env=env)
    missing = [str(p) for p in required if not p.is_file()]
    if missing:
        raise RuntimeError("Incomplete Qt installation: " + ", ".join(missing))
    marker.write_text(json.dumps(expected, indent=2) + "\n", encoding="utf-8")
    print(f"Installed Qt {VERSION} at {prefix}")


if __name__ == "__main__":
    main()
