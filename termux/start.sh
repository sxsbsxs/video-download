#!/data/data/com.termux/files/usr/bin/bash
set -euo pipefail
ROOT="$HOME/VideoDL"; mkdir -p "$ROOT/logs" "$HOME/storage/downloads/VideoDL"
if pgrep -f "termux_server/app.py" >/dev/null 2>&1; then echo "VideoDL 服务似乎已经运行"; exit 0; fi
termux-wake-lock 2>/dev/null || true
export PYTHONUNBUFFERED=1 VIDEODL_DOWNLOADS="$HOME/storage/downloads/VideoDL"
nohup python "$ROOT/termux_server/app.py" >> "$ROOT/logs/server.log" 2>&1 &
echo $! > "$ROOT/server.pid"; sleep 2
echo "本地服务：http://127.0.0.1:8765"; echo "访问密钥（不要公开分享）："
cat "$ROOT/termux_server/data/access.key"; echo
echo "另开会话运行：bash ~/VideoDL/termux/tunnel.sh"
