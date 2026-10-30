#!/usr/bin/env bash
# Runs FidelityFX_SC.exe under Wine for a Windows build cross-compiled on Linux. DXC under Wine
# can't resolve includes against Linux paths like "/home/...", so turn every absolute path argument
# into its Z: drive form first, and turn the depfile's paths back afterwards. Usage: ffx_sc_wine.sh <wine> <FidelityFX_SC.exe> <args...>
set -euo pipefail

to_windows() {
  local path="$1"
  printf 'Z:%s' "${path//\//\\}"
}

wine="$1"
exe="$2"
shift 2
args=()
output_dir=""
name=""
for arg in "$@"; do
  case "$arg" in
    -output=*) output_dir="${arg#-output=}" ;;&
    -name=*) name="${arg#-name=}" ;;&
    "-I "/*) args+=("-I$(to_windows "${arg#-I }")") ;;
    -I/*) args+=("-I$(to_windows "${arg#-I}")") ;;
    -output=/*) args+=("-output=$(to_windows "${arg#-output=}")") ;;
    /*) args+=("$(to_windows "$arg")") ;;
    *) args+=("$arg") ;;
  esac
done
"$wine" "$exe" "${args[@]}"

# The depfile (-deps=gcc) names Z:/ paths, which Ninja never finds, so it would rebuild every time.
depfile="$output_dir/${name}_permutations.h.d"
if [[ -n "$output_dir" && -n "$name" && -f "$depfile" ]]; then
  sed -i 's#Z:/#/#g' "$depfile"
fi
