#!/usr/bin/env bash
# Bash 3.2+; Windows runs under Git Bash in the matching compiler environment.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
fail() { echo "Error: $*" >&2; exit 2; }
usage() {
  cat <<'EOF'
Usage: ./build.sh [options]
  --platform macos|windows|linux   Default: current host
  --arch ARCH                    x86_64/amd64, arm64/aarch64, x86/386,
                                 Linux: arm, loong64, mips64le, riscv64
  --format dmg|exe|msi|both|deb|rpm Default: dmg / both / distro native
  --qt-prefix DIR                Installed target Qt prefix
  --qt-major 5|6                 Default: 5
  --build-qt                     Build pinned Qt 5 first (macOS/Windows)
  --qt-host-bins DIR             Native Qt 5 tools for macOS cross-build
  --go FILE                      Go executable (default: go)
  --build-dir DIR                CMake build directory
  --output DIR                   Fresh delivery directory (never overwritten)
  --version X.Y.Z                Default: CMake project version
  --deployment-target VERSION   macOS default: Intel 10.15 / ARM 11.0
  --jobs N                       Default: 4
  --generator NAME               Optional CMake generator, e.g. Ninja
  --cmake-arg KEY=VALUE           Repeatable toolchain/compiler settings
  --openssl-dir DIR              Windows Qt OpenSSL DLLs; otherwise native TLS
  --sign                        Request signing; missing certificates skip signing
  --notarize                    macOS notarization (implies --sign)
  --dry-run                     Print commands without building or writing files
  -h, --help

Run on the target OS (Linux inside its intended distro/ABI). macOS supports
cross-building Intel/ARM with matching Qt libraries and native Qt host tools.
Windows ARM64 is not supported by the existing protocol helper.
EOF
}
case "$(uname -s)" in
  Darwin) host=macos;; Linux) host=linux;; MINGW*|MSYS*|CYGWIN*) host=windows;; *) fail 'Unsupported host';;
esac
normalize_arch() {
  case "$1" in
    x86_64|amd64) echo amd64;; arm64|aarch64) echo arm64;; x86|i386|i686|386) echo 386;;
    arm|armv7l|armv6l) echo arm;; loong64|loongarch64) echo loong64;;
    mips64le|mips64el) echo mips64le;; riscv64) echo riscv64;; *) fail "Unsupported architecture: $1";;
  esac
}
host_arch=$(normalize_arch "$(uname -m)")
platform=$host arch=$host_arch format= qt_prefix= qt_major=5 qt_host_bins= go=go
build_dir= output= version= deployment= jobs=4 generator= openssl_dir=
sign=false notarize=false dry_run=false build_qt=false
cmake_args=()
while (($#)); do
  case "$1" in
    -h|--help) usage; exit 0;;
    --sign) sign=true; shift; continue;;
    --notarize) notarize=true; sign=true; shift; continue;;
    --dry-run) dry_run=true; shift; continue;;
    --build-qt) build_qt=true; shift; continue;;
    --platform|--arch|--format|--qt-prefix|--qt-major|--qt-host-bins|--go|--build-dir|--output|--version|--deployment-target|--jobs|--generator|--cmake-arg|--openssl-dir)
      (($# >= 2)) && [[ -n "$2" && "$2" != --* ]] || fail "Missing value for $1";;
    *) fail "Unknown argument: $1";;
  esac
  case "$1" in
    --platform) platform=$2;; --arch) arch=$(normalize_arch "$2");; --format) format=$2;;
    --qt-prefix) qt_prefix=$2;; --qt-major) qt_major=$2;; --qt-host-bins) qt_host_bins=$2;;
    --go) go=$2;; --build-dir) build_dir=$2;; --output) output=$2;; --version) version=$2;;
    --deployment-target) deployment=$2;; --jobs) jobs=$2;; --generator) generator=$2;;
    --openssl-dir) openssl_dir=$2;;
    --cmake-arg) [[ "$2" == *=* && "$2" != -* ]] || fail '--cmake-arg expects KEY=VALUE'; cmake_args+=("$2");;
  esac
  shift 2
done
[[ "$platform" == "$host" ]] || fail 'Use a target-system host/container; changing --platform does not cross-compile an OS'
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || fail '--jobs must be positive'
[[ "$qt_major" == 5 || "$qt_major" == 6 ]] || fail '--qt-major must be 5 or 6'
python=${PYTHON:-python3}
command -v "$python" >/dev/null || fail "Missing Python: $python (set PYTHON to its executable)"
if [[ -z "$version" ]]; then
  version=$("$python" -c 'import re,sys; print(re.search(r"set\(CLIENT_VERSION \"([0-9.]+)\"",open(sys.argv[1]).read())[1])' "$ROOT/CMakeLists.txt")
fi
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail 'Version must be X.Y.Z'
[[ -n "$build_dir" ]] || build_dir="$ROOT/build-$platform-$arch-qt$qt_major"
[[ -n "$output" ]] || output="$ROOT/dist/$platform-$arch-$(date +%Y%m%d-%H%M%S)"
[[ ! -e "$output" ]] || fail "Output already exists: $output"
run() { if $dry_run; then printf '%q ' "$@"; printf '\n'; else "$@"; fi; }
cross=false
if [[ "$arch" != "$host_arch" ]]; then
  if [[ "$platform" == macos ]]; then
    cross=true
    [[ -n "$qt_host_bins" ]] || fail 'macOS cross-build needs --qt-host-bins'
  elif [[ "$platform" != windows || "$host_arch" != amd64 || "$arch" != 386 ]]; then
    fail 'Use a matching architecture host/container (Windows x64 can also build x86)'
  fi
fi
case "$platform" in
  macos)
    [[ "$arch" == amd64 || "$arch" == arm64 ]] || fail 'macOS supports amd64/arm64'
    [[ -n "$format" ]] || format=dmg
    [[ "$format" == dmg ]] || fail 'macOS format must be dmg'
    [[ -n "$qt_prefix" ]] || fail 'macOS requires --qt-prefix built for the target OS baseline'
    qt_arch=$arch; [[ "$arch" != amd64 ]] || qt_arch=x86_64
    dmg_arch=x64; [[ "$arch" != arm64 ]] || dmg_arch=aarch64
    [[ -n "$deployment" ]] || { if [[ "$arch" == amd64 ]]; then deployment=10.15; else deployment=11.0; fi; }
    ;;
  windows)
    [[ "$arch" == amd64 || "$arch" == 386 ]] || fail 'Windows helper supports amd64/386'
    [[ -n "$format" ]] || format=both
    [[ "$format" == exe || "$format" == msi || "$format" == both ]] || fail 'Windows format must be exe/msi/both'
    [[ -n "$qt_prefix" ]] || fail 'Windows requires --qt-prefix'
    qt_arch=x86_64; [[ "$arch" != 386 ]] || qt_arch=x86
    ;;
  linux)
    [[ -n "$format" ]] || { if command -v dpkg-query >/dev/null; then format=deb; else format=rpm; fi; }
    [[ "$format" == deb || "$format" == rpm ]] || fail 'Linux format must be deb/rpm'
    ;;
esac
if [[ "$platform" != macos ]]; then
  [[ -z "$deployment" && -z "$qt_host_bins" ]] || fail 'macOS-only option used on another platform'
  $notarize && fail '--notarize is macOS-only'
fi
if [[ "$platform" != windows && -n "$openssl_dir" ]]; then fail '--openssl-dir is Windows-only'; fi
if ! $dry_run; then
  command -v "$go" >/dev/null || fail "Go executable not found: $go. Replace example paths with real paths, or omit --go to use PATH."
  if [[ -n "$qt_prefix" ]] && ! $build_qt; then
    [[ -f "$qt_prefix/lib/cmake/Qt$qt_major/Qt${qt_major}Config.cmake" || -f "$qt_prefix/lib64/cmake/Qt$qt_major/Qt${qt_major}Config.cmake" ]] || \
      fail "Qt $qt_major CMake configuration not found under $qt_prefix. Replace the example path with the installed target Qt prefix (containing lib/cmake/Qt$qt_major/Qt${qt_major}Config.cmake)."
  fi
fi
if $build_qt; then
  [[ "$platform" != linux && "$qt_major" == 5 ]] || fail '--build-qt supports pinned Qt 5 on macOS/Windows'
  qt_cmd=("$python" "$ROOT/scripts/build-qt.py" --prefix "$qt_prefix" --work "$build_dir-qt-source" --arch "$qt_arch" --parallel "$jobs")
  [[ -z "$deployment" ]] || qt_cmd+=(--deployment-target "$deployment")
  [[ -z "$qt_host_bins" ]] || qt_cmd+=(--host-bindir "$qt_host_bins")
  run "${qt_cmd[@]}"
fi
if [[ "$platform" == linux ]]; then
  $sign && fail 'Linux package signing is not configured; use distro repository signing separately'
  cmd=("$python" "$ROOT/scripts/package-linux.py" --format "$format" --build "$build_dir" --output "$output" --go "$go" --qt-major "$qt_major" --parallel "$jobs" --cmake-arg "CLIENT_VERSION=$version")
  [[ -z "$qt_prefix" ]] || cmd+=(--cmake-arg "CMAKE_PREFIX_PATH=$qt_prefix")
  [[ -z "$generator" ]] || fail 'Linux generator is managed by package-linux.py'
  for option in ${cmake_args[@]+"${cmake_args[@]}"}; do cmd+=(--cmake-arg "$option"); done
  run "${cmd[@]}"
  exit 0
fi
configure=(cmake -S "$ROOT" -B "$build_dir" "-DCLIENT_VERSION=$version" "-DCLIENT_QT_MAJOR=$qt_major" "-DCMAKE_PREFIX_PATH=$qt_prefix" "-DGO_EXECUTABLE=$go" "-DCLIENT_GOARCH=$arch" -DCMAKE_BUILD_TYPE=Release)
[[ -z "$generator" ]] || configure+=(-G "$generator")
if [[ "$platform" == macos ]]; then
  configure+=("-DCMAKE_OSX_ARCHITECTURES=$qt_arch" "-DCMAKE_OSX_DEPLOYMENT_TARGET=$deployment")
  [[ -z "$qt_host_bins" ]] || configure+=("-DCLIENT_QT_HOST_BINS=$qt_host_bins")
fi
for option in ${cmake_args[@]+"${cmake_args[@]}"}; do configure+=("-D$option"); done
run "${configure[@]}"
run cmake --build "$build_dir" --config Release --parallel "$jobs"
if [[ "$platform" == windows ]]; then
  cmd=("$python" "$ROOT/scripts/package-windows.py" --build "$build_dir" --arch "$arch" --format "$format" --output "$output" --windeployqt "$qt_prefix/bin/windeployqt.exe" --skip-build)
  if [[ -n "$openssl_dir" ]]; then cmd+=(--openssl-dir "$openssl_dir"); else cmd+=(--native-tls); fi
  $sign && cmd+=(--sign)
  run "${cmd[@]}"
else
  signing_env=
  cleanup() {
    if [[ -n "$signing_env" ]]; then
      while IFS='=' read -r key value; do
        case "$key" in APPLE_KEYCHAIN_STATE) export "$key=$value";; esac
      done < "$signing_env"
      "$python" "$ROOT/scripts/setup-apple-cert.py" cleanup
      rm -f -- "$signing_env"
    fi
  }
  trap cleanup EXIT
  if $sign && [[ -z "${APPLE_SIGNING_IDENTITY:-}" ]] && ! $dry_run; then
    signing_env=$(mktemp)
    GITHUB_ENV="$signing_env" "$python" "$ROOT/scripts/setup-apple-cert.py" setup
    while IFS='=' read -r key value; do
      case "$key" in APPLE_SIGNING_ENABLED|APPLE_SIGNING_IDENTITY|APPLE_SIGNING_KEYCHAIN|APPLE_KEYCHAIN_STATE) export "$key=$value";; esac
    done < "$signing_env"
  fi
  deployqt="$qt_prefix/bin/macdeployqt"
  cmd=("$python" "$ROOT/scripts/package-macos.py" --build "$build_dir" --macdeployqt "$deployqt" --output "$output/OSMClient.app" --deployment-target "$deployment")
  if $cross; then cmd+=(--plugins-dir "$qt_prefix/plugins" --libpath "$qt_prefix/lib" --skip-smoke); fi
  $sign && cmd+=(--sign)
  run "${cmd[@]}"
  cmd=("$python" "$ROOT/scripts/package-dmg.py" --app "$output/OSMClient.app" --output "$output/OSMClient_${version}_${dmg_arch}.dmg" --deployment-target "$deployment")
  $cross && cmd+=(--skip-smoke)
  $sign && cmd+=(--sign)
  $notarize && cmd+=(--notarize)
  run "${cmd[@]}"
fi
