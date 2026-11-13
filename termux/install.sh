#!/data/data/com.termux/files/usr/bin/bash
set -euo pipefail
ROOT="$HOME/VideoDL"
echo "[1/4] 更新软件包"; pkg update -y; pkg upgrade -y
echo "[2/4] 安装 Python、FFmpeg、Cloudflare Tunnel"; pkg install -y python ffmpeg cloudflared termux-services
echo "[3/4] 申请手机共享存储权限"; termux-setup-storage || true
echo "[4/4] 安装 Python 依赖"
python -m pip install --upgrade pip
python -m pip install -r "$ROOT/termux_server/requirements.txt"
mkdir -p "$ROOT/logs" "$HOME/storage/downloads/VideoDL"
echo "安装完成。运行：bash ~/VideoDL/termux/start.sh"
