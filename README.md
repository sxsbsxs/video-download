# VideoDL Next

一个以 **C++ API + Python / yt-dlp + HTML** 构成的轻量视频链接解析网站。前端负责输入链接、展示视频信息、封面和可用格式；后端负责校验请求并调用 yt-dlp 提取媒体信息。默认把用户带到媒体源站下载，不把大视频文件经由免费服务器中转。

> 这是一个可自托管的开源项目。请只解析和下载你有权访问、且允许下载的内容。项目不提供 DRM、付费墙、登录限制或其他访问控制的绕过功能。

## 功能

- 纯 HTML / CSS / JavaScript 前端，无需 Node 前端构建流程。
- C++ HTTP API：健康检查、输入校验、请求限速和并发上限。
- Python 调用 yt-dlp，支持其所覆盖的网站和格式。
- 提取标题、封面、作者、时长、视频清晰度、编码、格式和源站直链。
- 显示可用格式，支持打开源站媒体链接、复制链接。
- 本地浏览器保存最近解析记录，不把历史记录上传到服务器。
- Node.js 22 + yt-dlp EJS 脚本，为需要 JavaScript 挑战求解的网站提供运行环境。
- Docker 多阶段构建、Render Blueprint 和 GitHub Actions 自动构建检查。

## 项目结构

    .
    ├── CMakeLists.txt
    ├── Dockerfile
    ├── docker-compose.yml
    ├── render.yaml
    ├── requirements.txt
    ├── src/main.cpp
    ├── worker/extract.py
    ├── public/index.html
    └── .github/workflows/ci.yml

## 本地运行（Docker，推荐）

需要安装 Docker Engine 或 Docker Desktop。

    git clone https://github.com/sxsbsxs/video-download.git
    cd video-download
    docker compose up --build

打开 http://localhost:10000。

停止：

    docker compose down

首次构建需要下载基础镜像和 Python 依赖，因此需要可用的网络连接。

## 本地编译（Debian / Ubuntu）

需要 C++20 编译器、CMake、pkg-config、cpp-httplib、nlohmann-json、Python 3、FFmpeg 和 Node.js 22 或更高版本。

    sudo apt-get update
    sudo apt-get install -y build-essential cmake pkg-config libcpp-httplib-dev nlohmann-json3-dev python3 python3-venv ffmpeg

创建 Python 虚拟环境并安装解析引擎：

    python3 -m venv .venv
    . .venv/bin/activate
    pip install -U pip
    pip install -r requirements.txt

编译并启动：

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j2
    PORT=10000 PYTHON_BIN="$(pwd)/.venv/bin/python" WORKER_SCRIPT="$(pwd)/worker/extract.py" PUBLIC_DIR="$(pwd)/public" ./build/videodl

浏览器访问 http://localhost:10000。

## Render 免费部署

仓库已包含 render.yaml。推荐通过 Render Blueprint 导入本仓库：

1. 登录 Render，选择 **New → Blueprint**。
2. 连接 GitHub，并选择 sxsbsxs/video-download。
3. Render 读取仓库根目录的 render.yaml，检查服务名称、Docker 构建和 Singapore 区域。
4. 确认计划为 Free 后创建服务。
5. 首次构建完成后打开 Render 提供的 onrender.com 地址。

也可以在 Render 中选择 New → Web Service，连接本仓库，Runtime 选择 Docker，Plan 选择 Free，Health Check Path 填 /api/health。

### 免费计划的重要限制

Render Free 适合个人测试和低流量预览，不应当被当作稳定生产服务器：

- 连续 15 分钟没有入站流量时会休眠；再次访问通常需要约一分钟唤醒。
- 免费实例的文件系统是临时的，重启、休眠或重新部署后本地改动会丢失。
- 单实例约 512 MB RAM、0.1 CPU；复杂站点解析或高并发可能失败。
- 每个工作区每月有 750 小时免费实例时长，以及带宽和构建分钟数限制。
- 没有添加付款方式时，超出部分免费额度可能导致服务暂停或无法继续构建；具体资格和账单页面以 Render 当前账户显示为准。
- 免费服务可能被 Render 重启，不支持持久磁盘和 SSH。

**不保证 Render 永久免费、免绑卡、长期在线或中国大陆稳定访问。** 部署前请在 Render 的确认页面核实最终费用和是否要求付款方式。Singapore 是一个可选部署区域，但跨境网络质量由访问者网络和服务商路由决定，不能承诺所有大陆网络都快。

## API

### GET /api/health

返回服务状态，可用于平台健康检查。

### POST /api/extract

请求示意：

    {
      "url": "https://example.com/watch/..."
    }

成功响应包含视频标题、封面、格式数组和直链 URL。错误响应包含 ok=false 和可读的 error 信息。API 限制请求体大小、每个 IP 的解析频率和同时运行的解析任务数。请求最长运行约 50 秒。

## 下载行为和限制

本项目是**解析器，不是媒体代理或云端转码服务**。后端返回网站提供的媒体直链，浏览器随后直接访问源站。这样能节约免费服务器出站流量，但也意味着：

- 媒体直链可能有时效，过一段时间后需要重新解析。
- 某些网站的直链要求特定请求头、Cookie 或登录状态，浏览器直接打开可能失败。
- DASH 网站可能把视频和音频拆成两条流；本项目显示可用流，不会在免费实例上自动合并。
- 部分网站可能返回 403、412、地区限制或登录提示。项目不会尝试绕过这些限制。
- 解析成功不代表该链接一定能被浏览器直接保存；最终由源站策略和浏览器行为决定。

## 部署安全提示

当前版本包含 URL 协议校验、DNS 解析后的私网地址拦截、请求限速、并发限制和执行超时。这些是基础防护，不等于完整的生产级沙箱：站点重定向、DNS 变化和第三方提取器行为仍会带来残余 SSRF 风险。

不要在未加额外访问控制、网络出口隔离和监控的情况下，将这个免费演示实例用于高风险或高流量的公共服务。正式公开部署前，应在反向代理或 WAF 层增加访问限制、滥用检测和配额。

## 许可证

本项目自有代码采用 MIT License。yt-dlp、FFmpeg、cpp-httplib、nlohmann-json 和 Node.js 均为独立第三方项目，分别遵循其自己的许可证；本仓库不重新分发这些软件的源码或二进制。
