#!/usr/bin/env sh
# ==================================================================================================
# CoralGraphics standalone build
# ==================================================================================================
# Configures, builds and tests this repository as a standalone project:
#
#   ./scripts/build.sh                 # Release, tests on
#   ./scripts/build.sh --debug         # Debug
#   ./scripts/build.sh --no-tests      # skip the suites (and GoogleTest)
#   ./scripts/build.sh --clean         # remove the build directory first
#
# A dependency declared in .gitmodules is fetched automatically when it is missing, so a fresh
# checkout needs one command. Rerun ./scripts/bootstrap.sh explicitly to update the dependencies.
#
# Unknown arguments are passed to the CMake configure step, which is how a feature switch is set:
#
#   ./scripts/build.sh -- -DCORAL_BACKEND_SUPPORTS_METAL=OFF
#
# Options:
#   --debug | --release     build configuration (default Release)
#   --no-tests              configure with -DCORAL_BUILD_TESTS=OFF
#   --no-bootstrap          never fetch dependencies
#   --clean                 delete the build directory before configuring
#   --build-dir DIR         build directory (default build/)
#   --jobs N                parallel build jobs (default: number of CPUs)
#   -h | --help             this text
# ==================================================================================================

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

BUILD_TYPE=Release
BUILD_DIR="$ROOT_DIR/build"
TESTS=ON
BOOTSTRAP=1
CLEAN=0
JOBS=""
CMAKE_ARGS=""

say() { printf '%s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

usage() {
  sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
  exit 0
}

default_jobs() {
  if command -v nproc >/dev/null 2>&1; then
    nproc
  elif command -v sysctl >/dev/null 2>&1; then
    sysctl -n hw.ncpu 2>/dev/null || printf '4\n'
  else
    printf '4\n'
  fi
}

while [ $# -gt 0 ]; do
  case "$1" in
    --debug)   BUILD_TYPE=Debug ;;
    --release) BUILD_TYPE=Release ;;
    --no-tests) TESTS=OFF ;;
    --no-bootstrap) BOOTSTRAP=0 ;;
    --clean)   CLEAN=1 ;;
    --build-dir)
      [ $# -ge 2 ] || die "--build-dir needs a path"
      BUILD_DIR=$2
      shift
      ;;
    --jobs)
      [ $# -ge 2 ] || die "--jobs needs a count"
      JOBS=$2
      shift
      ;;
    -h|--help) usage ;;
    --) shift
      while [ $# -gt 0 ]; do
        CMAKE_ARGS="$CMAKE_ARGS $1"
        shift
      done
      break
      ;;
    *)
      # Everything else is a CMake configure argument, so feature switches do not need a second
      # command line.
      CMAKE_ARGS="$CMAKE_ARGS $1"
      ;;
  esac
  shift
done

case "$BUILD_DIR" in
  /*) ;;
  *) BUILD_DIR="$ROOT_DIR/$BUILD_DIR" ;;
esac
[ -n "$JOBS" ] || JOBS=$(default_jobs)

# --------------------------------------------------------------------------------------------------
# Dependencies
# --------------------------------------------------------------------------------------------------
dependencies_missing() {
  [ -f "$ROOT_DIR/external/CoralCore/CMakeLists.txt" ] || return 0
  [ -f "$ROOT_DIR/third_party/libz/coral/CMakeLists.txt" ] || return 0
  [ -f "$ROOT_DIR/third_party/libyuv/CMakeLists.txt" ] || return 0
  if [ "$TESTS" = ON ]; then
    [ -f "$ROOT_DIR/third_party/libgtest/coral/CMakeLists.txt" ] || return 0
  fi
  return 1
}

if dependencies_missing && [ "$BOOTSTRAP" = 1 ]; then
  say "Some dependencies declared in .gitmodules are missing; fetching them first."
  "$ROOT_DIR/scripts/bootstrap.sh"
elif dependencies_missing; then
  die "dependencies are missing and --no-bootstrap was given; run ./scripts/bootstrap.sh"
fi

# --------------------------------------------------------------------------------------------------
# Configure, build, test
# --------------------------------------------------------------------------------------------------
command -v cmake >/dev/null 2>&1 || die "cmake is required"

if [ "$CLEAN" = 1 ] && [ -d "$BUILD_DIR" ]; then
  say "Removing $BUILD_DIR"
  rm -rf "$BUILD_DIR"
fi

say "Configuring CoralGraphics ($BUILD_TYPE, tests=$TESTS)"
# shellcheck disable=SC2086
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DCORAL_BUILD_TESTS="$TESTS" \
  $CMAKE_ARGS

say "Building with $JOBS job(s)"
cmake --build "$BUILD_DIR" --parallel "$JOBS"

if [ "$TESTS" = ON ]; then
  say "Running the test suites"
  # Only CoralGraphics' own suites and demos. A standalone configure also adds the dependency
  # repositories' tests (libjpeg-turbo's command-line checks among them), and several of those are
  # registered without the executables this build produces; `ctest` reports them as Not Run, which
  # is not a failure of this repository. The named set is the framework suites plus the demos the
  # examples project registers.
  TEST_FILTER='^(test_(graphics|backend|geometry|effect)|effect_|mac_demo_verify|graphics_demo_|vulkan_demo_|linux_demo_|lottie_player_|fuzz_parsers_smoke)'
  ctest --test-dir "$BUILD_DIR" -R "$TEST_FILTER" --output-on-failure
fi

say ""
say "Done. The build is in $BUILD_DIR"
