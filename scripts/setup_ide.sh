#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# setup_ide.sh — 在任意 macOS / Linux 机器上一键准备 IDE（clangd）解析环境。
#
# 为什么需要这一步：
#   compile_commands.json（clangd 的编译数据库）记录的是【本机绝对路径】，
#   不随仓库提交（已在 .gitignore 中忽略）。任何新机器 clone 后都必须重新
#   configure 一次来生成它，clangd 才能正确解析、跳转、查找引用。
#
# 用法：
#   ./scripts/setup_ide.sh            # 默认 Release configure
#   BUILD_TYPE=Debug ./scripts/setup_ide.sh
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

# 切到仓库根目录（脚本所在目录的上一级）
cd "$(dirname "$0")/.."

BUILD_DIR="${BUILD_DIR:-build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

echo "==> 检查 cmake ..."
if ! command -v cmake >/dev/null 2>&1; then
    echo "错误：未找到 cmake。请先安装：" >&2
    echo "  macOS:  brew install cmake" >&2
    echo "  Linux:  sudo apt install cmake   (或对应发行版包管理器)" >&2
    exit 1
fi

echo "==> 检查 clangd ..."
if ! command -v clangd >/dev/null 2>&1; then
    echo "警告：未找到 clangd（IDE 解析依赖它）。建议安装：" >&2
    echo "  macOS:  自带于 CommandLineTools（xcode-select --install）或 brew install llvm" >&2
    echo "  Linux:  sudo apt install clangd   (或 clangd-<version>)" >&2
    echo "  VS Code 安装 'clangd' 扩展（llvm-vs-code-extensions.vscode-clangd）" >&2
fi

echo "==> CMake configure（生成 ${BUILD_DIR}/compile_commands.json）..."
cmake -S . -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

# CMakeLists.txt 中的 sync_compile_commands 目标会在首次 build 时把
# compile_commands.json 拷到仓库根目录；这里直接拷一份，免去先 build。
if [ -f "${BUILD_DIR}/compile_commands.json" ]; then
    cp -f "${BUILD_DIR}/compile_commands.json" ./compile_commands.json
    echo "==> 已生成 compile_commands.json（build/ 与仓库根各一份）"
fi

echo ""
echo "✅ 完成。请在编辑器中重启 clangd："
echo "   VS Code: Cmd/Ctrl+Shift+P → 'clangd: Restart language server'"
echo "   首次会后台索引整个项目，索引完成后跳转 / 查找引用即可正常工作。"
