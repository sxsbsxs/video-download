#!/data/data/com.termux/files/usr/bin/bash
set -e
termux-wake-lock
bash "$HOME/VideoDL/termux/start.sh"
sleep 4
nohup cloudflared tunnel --no-autoupdate --url http://127.0.0.1:8765 >> "$HOME/VideoDL/logs/tunnel.log" 2>&1 &
