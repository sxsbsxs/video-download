#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace {
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

std::atomic<int> active_jobs{0};
std::mutex rate_mutex;
std::unordered_map<std::string, std::deque<Clock::time_point>> request_times;

std::string envOr(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return value && *value ? value : fallback;
}

bool isUnsafeIPv4(std::uint32_t address) {
    const auto in = [address](std::uint32_t network, std::uint32_t mask) {
        return (address & mask) == network;
    };
    return in(0x00000000U, 0xFF000000U) ||
           in(0x0A000000U, 0xFF000000U) ||
           in(0x64400000U, 0xFFC00000U) ||
           in(0x7F000000U, 0xFF000000U) ||
           in(0xA9FE0000U, 0xFFFF0000U) ||
           in(0xAC100000U, 0xFFF00000U) ||
           in(0xC0000000U, 0xFFFFFF00U) ||
           in(0xC0000200U, 0xFFFFFF00U) ||
           in(0xC0A80000U, 0xFFFF0000U) ||
           in(0xC0586300U, 0xFFFFFF00U) ||
           in(0xC6120000U, 0xFFFE0000U) ||
           in(0xC6336400U, 0xFFFFFF00U) ||
           in(0xCB007100U, 0xFFFFFF00U) ||
           in(0xE0000000U, 0xF0000000U) ||
           in(0xF0000000U, 0xF0000000U);
}

bool isUnsafeIPv6(const in6_addr& address) {
    if (IN6_IS_ADDR_UNSPECIFIED(&address) ||
        IN6_IS_ADDR_LOOPBACK(&address) ||
        IN6_IS_ADDR_LINKLOCAL(&address) ||
        IN6_IS_ADDR_MULTICAST(&address)) {
        return true;
    }
    if ((address.s6_addr[0] & 0xFEU) == 0xFCU) {
        return true;
    }
    if (address.s6_addr[0] == 0x20U && address.s6_addr[1] == 0x01U &&
        address.s6_addr[2] == 0x0DU && address.s6_addr[3] == 0xB8U) {
        return true;
    }
    if (IN6_IS_ADDR_V4MAPPED(&address)) {
        std::uint32_t v4 = 0;
        std::memcpy(&v4, &address.s6_addr[12], sizeof(v4));
        return isUnsafeIPv4(ntohl(v4));
    }
    return false;
}

bool hostResolvesOnlyToPublicAddresses(const std::string& host) {
    in_addr v4{};
    if (inet_pton(AF_INET, host.c_str(), &v4) == 1) {
        return !isUnsafeIPv4(ntohl(v4.s_addr));
    }

    in6_addr v6{};
    if (inet_pton(AF_INET6, host.c_str(), &v6) == 1) {
        return !isUnsafeIPv6(v6);
    }

    if (host.empty() || host == "localhost" || host.ends_with(".localhost") ||
        host.ends_with(".local") || host == "metadata.google.internal") {
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &results) != 0 || !results) {
        return false;
    }

    bool found = false;
    bool safe = true;
    for (addrinfo* item = results; item; item = item->ai_next) {
        if (item->ai_family == AF_INET) {
            const auto* value = reinterpret_cast<const sockaddr_in*>(item->ai_addr);
            found = true;
            safe = safe && !isUnsafeIPv4(ntohl(value->sin_addr.s_addr));
        } else if (item->ai_family == AF_INET6) {
            const auto* value = reinterpret_cast<const sockaddr_in6*>(item->ai_addr);
            found = true;
            safe = safe && !isUnsafeIPv6(value->sin6_addr);
        }
    }
    freeaddrinfo(results);
    return found && safe;
}

bool validateUrl(const std::string& url, std::string& reason) {
    if (url.empty() || url.size() > 8192) {
        reason = "链接为空或长度超过限制。";
        return false;
    }
    for (unsigned char ch : url) {
        if (std::iscntrl(ch) || std::isspace(ch)) {
            reason = "链接中不能包含空白或控制字符。";
            return false;
        }
    }

    static const std::regex pattern(R"(^(https?)://([^/?#]+))", std::regex::icase);
    std::smatch match;
    if (!std::regex_search(url, match, pattern)) {
        reason = "只支持以 http:// 或 https:// 开头的链接。";
        return false;
    }
    std::string authority = match[2].str();
    if (authority.find('@') != std::string::npos) {
        reason = "不接受包含用户名或密码的链接。";
        return false;
    }

    std::string host;
    std::string port;
    if (!authority.empty() && authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string::npos) {
            reason = "IPv6 主机地址格式不正确。";
            return false;
        }
        host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') {
                reason = "主机地址格式不正确。";
                return false;
            }
            port = authority.substr(close + 2);
        }
    } else {
        const auto colon = authority.find(':');
        if (colon != std::string::npos) {
            if (authority.find(':', colon + 1) != std::string::npos) {
                reason = "IPv6 地址必须使用方括号。";
                return false;
            }
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
        } else {
            host = authority;
        }
    }

    if (host.empty() || host.find('%') != std::string::npos) {
        reason = "主机名格式不正确。";
        return false;
    }
    if (!port.empty()) {
        if (!std::all_of(port.begin(), port.end(), [](unsigned char c) { return std::isdigit(c); })) {
            reason = "端口格式不正确。";
            return false;
        }
        try {
            const auto number = std::stoul(port);
            if (number == 0 || number > 65535) {
                reason = "端口超出有效范围。";
                return false;
            }
        } catch (...) {
            reason = "端口格式不正确。";
            return false;
        }
    }

    std::transform(host.begin(), host.end(), host.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!hostResolvesOnlyToPublicAddresses(host)) {
        reason = "目标主机无法解析，或属于本机、内网及保留地址范围。";
        return false;
    }
    return true;
}

bool allowRequest(const std::string& ip) {
    const auto now = Clock::now();
    std::lock_guard<std::mutex> guard(rate_mutex);
    auto& times = request_times[ip];
    while (!times.empty() && now - times.front() > std::chrono::minutes(1)) {
        times.pop_front();
    }
    if (times.size() >= 8) {
        return false;
    }
    times.push_back(now);
    if (request_times.size() > 4000) {
        for (auto it = request_times.begin(); it != request_times.end();) {
            if (it->second.empty() || now - it->second.back() > std::chrono::minutes(10)) {
                it = request_times.erase(it);
            } else {
                ++it;
            }
        }
    }
    return true;
}

struct WorkerResult {
    bool started = false;
    bool timed_out = false;
    int exit_code = -1;
    std::string output;
    std::string error;
};

WorkerResult runExtractor(const std::string& python, const std::string& script,
                          const std::string& url) {
    WorkerResult result;
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        result.error = "无法创建解析进程管道。";
        return result;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        result.error = "无法启动解析进程。";
        return result;
    }

    if (pid == 0) {
        setsid();
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        const int nullfd = open("/dev/null", O_WRONLY);
        if (nullfd >= 0) {
            dup2(nullfd, STDERR_FILENO);
            close(nullfd);
        }
        execl(python.c_str(), python.c_str(), script.c_str(), "--url", url.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }

    result.started = true;
    close(pipefd[1]);
    const int flags = fcntl(pipefd[0], F_GETFL, 0);
    if (flags >= 0) {
        fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
    }

    bool eof = false;
    bool child_done = false;
    int status = 0;
    const auto deadline = Clock::now() + std::chrono::seconds(50);
    constexpr std::size_t max_output = 2U * 1024U * 1024U;
    char buffer[8192];

    while (!eof || !child_done) {
        if (Clock::now() > deadline) {
            result.timed_out = true;
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            child_done = true;
            break;
        }

        pollfd descriptor{};
        descriptor.fd = pipefd[0];
        descriptor.events = POLLIN | POLLHUP;
        poll(&descriptor, 1, 250);

        while (!eof) {
            const ssize_t count = read(pipefd[0], buffer, sizeof(buffer));
            if (count > 0) {
                result.output.append(buffer, static_cast<std::size_t>(count));
                if (result.output.size() > max_output) {
                    kill(pid, SIGKILL);
                    waitpid(pid, &status, 0);
                    close(pipefd[0]);
                    result.error = "解析结果超过大小限制。";
                    return result;
                }
            } else if (count == 0) {
                eof = true;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                break;
            } else {
                eof = true;
            }
        }

        if (!child_done) {
            const pid_t waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid) {
                child_done = true;
            }
        }
    }

    close(pipefd[0]);
    if (result.timed_out) {
        result.error = "解析超时（约 50 秒）。目标网站响应过慢或当前免费实例资源不足。";
        return result;
    }
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }
    return result;
}

void setCommonHeaders(httplib::Response& response) {
    response.set_header("X-Content-Type-Options", "nosniff");
    response.set_header("Referrer-Policy", "strict-origin-when-cross-origin");
    response.set_header("Cache-Control", "no-store");
}

void sendJson(httplib::Response& response, int status, const json& body) {
    response.status = status;
    response.set_content(body.dump(), "application/json; charset=utf-8");
    setCommonHeaders(response);
}

} // namespace

int main() {
    const std::string python = envOr("PYTHON_BIN", "/opt/venv/bin/python");
    const std::string script = envOr("WORKER_SCRIPT", "/app/worker/extract.py");
    const std::string public_dir = envOr("PUBLIC_DIR", "/app/public");
    int port = 10000;
    try {
        const int configured = std::stoi(envOr("PORT", "10000"));
        if (configured > 0 && configured <= 65535) {
            port = configured;
        }
    } catch (...) {
        std::cerr << "Invalid PORT; using 10000.\n";
    }

    if (!std::filesystem::exists(script)) {
        std::cerr << "Worker script not found: " << script << "\n";
        return 2;
    }
    const std::string index_path = public_dir + "/index.html";
    if (!std::filesystem::exists(index_path)) {
        std::cerr << "Frontend not found: " << index_path << "\n";
        return 2;
    }

    httplib::Server server;
    server.set_payload_max_length(16U * 1024U);
    server.set_read_timeout(5, 0);
    server.set_write_timeout(10, 0);
    server.new_task_queue = [] { return new httplib::ThreadPool(8); };

    server.Get("/", [index_path](const httplib::Request&, httplib::Response& response) {
        std::ifstream input(index_path, std::ios::binary);
        if (!input) {
            sendJson(response, 500, {{"ok", false}, {"error", "前端文件暂不可用。"}});
            return;
        }
        std::string html((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
        response.set_content(html, "text/html; charset=utf-8");
        response.set_header("X-Content-Type-Options", "nosniff");
        response.set_header("Referrer-Policy", "strict-origin-when-cross-origin");
        response.set_header("Cache-Control", "no-cache");
    });

    server.Get("/api/health", [](const httplib::Request&, httplib::Response& response) {
        sendJson(response, 200, {
            {"ok", true},
            {"service", "VideoDL Next"},
            {"engine", "yt-dlp"},
            {"status", "ready"}
        });
    });

    server.Get("/robots.txt", [](const httplib::Request&, httplib::Response& response) {
        response.set_content("User-agent: *\nDisallow: /api/\n", "text/plain; charset=utf-8");
        setCommonHeaders(response);
    });

    server.Get("/favicon.ico", [](const httplib::Request&, httplib::Response& response) {
        response.status = 204;
        setCommonHeaders(response);
    });

    server.Post("/api/extract", [python, script](const httplib::Request& request,
                                                httplib::Response& response) {
        if (!allowRequest(request.remote_addr)) {
            sendJson(response, 429, {
                {"ok", false},
                {"error", "请求过于频繁，请稍后再试（每个 IP 每分钟最多 8 次）。"}
            });
            return;
        }

        json input;
        try {
            input = json::parse(request.body);
        } catch (...) {
            sendJson(response, 400, {{"ok", false}, {"error", "请求内容不是有效 JSON。"}});
            return;
        }
        if (!input.is_object() || !input.contains("url") || !input["url"].is_string()) {
            sendJson(response, 400, {{"ok", false}, {"error", "请提供 url 字段。"}});
            return;
        }

        const std::string url = input["url"].get<std::string>();
        std::string reason;
        if (!validateUrl(url, reason)) {
            sendJson(response, 400, {{"ok", false}, {"error", reason}});
            return;
        }

        const int current = active_jobs.fetch_add(1);
        if (current >= 2) {
            active_jobs.fetch_sub(1);
            sendJson(response, 503, {
                {"ok", false},
                {"error", "当前解析任务较多，请稍后重试。免费实例最多同时解析 2 个链接。"}
            });
            return;
        }

        struct JobGuard {
            ~JobGuard() { active_jobs.fetch_sub(1); }
        } guard;

        WorkerResult worker = runExtractor(python, script, url);
        if (!worker.error.empty()) {
            sendJson(response, 504, {{"ok", false}, {"error", worker.error}});
            return;
        }
        if (worker.output.empty()) {
            sendJson(response, 502, {{"ok", false}, {"error", "解析引擎没有返回结果，请稍后重试。"}});
            return;
        }

        try {
            json output = json::parse(worker.output);
            if (!output.is_object() || !output.value("ok", false)) {
                std::string message = "目标网站暂时无法解析该链接。";
                if (output.is_object() && output.contains("error") && output["error"].is_string()) {
                    message = output["error"].get<std::string>();
                }
                sendJson(response, 422, {{"ok", false}, {"error", message}});
                return;
            }
            sendJson(response, 200, output);
        } catch (...) {
            sendJson(response, 502, {{"ok", false}, {"error", "解析引擎返回的数据格式异常。"}});
        }
    });

    std::cout << "VideoDL Next listening on 0.0.0.0:" << port << std::endl;
    if (!server.listen("0.0.0.0", port)) {
        std::cerr << "Failed to listen on port " << port << "\n";
        return 1;
    }
    return 0;
}
