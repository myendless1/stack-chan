#!/usr/bin/env bash

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export IDF_PATH="$ROOT_DIR/esp-idf"
export IDF_TOOLS_PATH="$ROOT_DIR/.espressif"
export IDF_PYTHON_ENV_PATH="$ROOT_DIR/.venv"
export UV_CACHE_DIR="$ROOT_DIR/.uv-cache"
export XDG_CACHE_HOME="$ROOT_DIR/.cache"

export http_proxy="${http_proxy:-http://localhost:7890}"
export https_proxy="${https_proxy:-http://localhost:7890}"
export all_proxy="${all_proxy:-socks5://localhost:7891}"

. "$IDF_PATH/export.sh"
