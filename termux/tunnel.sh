#!/data/data/com.termux/files/usr/bin/bash
set -euo pipefail
if ! pgrep -f "termux_server/app.py" >/dev/null 2>&1; then echo "请先运行 bash ~/VideoDL/termux/start.sh"; exit 1; fi
echo "正在建立公网临时隧道；请复制随后显示的 trycloudflare.com 地址。"
exec cloudflared tunnel --no-autoupdate --url http://127.0.0.1:8765
