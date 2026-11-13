# VideoDL on Termux

Android 手机作为下载服务器。yt-dlp 在手机本地解析和下载，FFmpeg 合并音视频，Cloudflare Quick Tunnel 提供临时公网入口。

## 注意事项

- Quick Tunnel 生成随机 trycloudflare.com 地址，不需要域名或 Cloudflare 账号，但地址会变化，隧道停止即失效；它适合测试/临时分享，不提供 uptime 保证。
- 网页需要访问密钥。不要把密钥发到公开群聊；任何拿到密钥的人都能创建下载任务。
- 手机必须联网并保持 Termux 与 cloudflared 运行。Android 省电策略可能杀死后台进程。
- 下载流量由手机网络承担，会占用存储、电量和流量。请确认剩余空间。
- 仅下载你有权访问且平台允许下载的内容。

## 安装

在 Termux 执行：

    pkg update -y && pkg upgrade -y
    pkg install -y git
    git clone https://github.com/sxsbsxs/video-download.git ~/VideoDL
    cd ~/VideoDL
    bash termux/install.sh

## 启动

    bash ~/VideoDL/termux/start.sh

复制终端显示的访问密钥。密钥保存在 ~/VideoDL/termux_server/data/access.key。

新开一个 Termux 会话：

    bash ~/VideoDL/termux/tunnel.sh

打开 cloudflared 输出的 https://....trycloudflare.com 地址，输入访问密钥。

## 开机自动启动（可选）

1. 安装 Termux:Boot，打开一次。
2. 执行：

    mkdir -p ~/.termux/boot
    cp ~/VideoDL/termux/boot/start-videodl.sh ~/.termux/boot/start-videodl
    chmod +x ~/.termux/boot/start-videodl

3. 在 Android 设置中将 Termux、Termux:Boot 设为不受电池优化限制。部分厂商仍可能结束后台进程。
4. 重启后查看：

    tail -f ~/VideoDL/logs/server.log
    tail -f ~/VideoDL/logs/tunnel.log

Quick Tunnel 重启后地址可能变化，从 tunnel.log 查找新地址。

## 停止

    pkill -f 'termux_server/app.py' || true
    pkill -f 'cloudflared tunnel' || true
    termux-wake-unlock

## 更新

    cd ~/VideoDL && git pull
    python -m pip install -r termux_server/requirements.txt

## API

- GET /api/health
- POST /api/extract（Header: X-Access-Key）
- POST /api/downloads（Header: X-Access-Key）
- GET /api/tasks（Header: X-Access-Key）
- GET /api/files/{id}（Header: X-Access-Key）

默认最多同时运行 2 个任务，可通过 VIDEODL_MAX_TASKS 调整，最大 3。
