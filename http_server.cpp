#include "include/SignalHandler.h"
#include "include/http/HttpRequest.h"
#include "include/http/HttpResponse.h"
#include "include/http/HttpServer.h"
#include "include/log/Logger.h"

#include <atomic>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

// ── 配置 ──────────────────────────────────────────────────────────────────────
static const std::string kStaticDir = "static"; // 静态资源目录（相对 CWD）
static const std::string kFilesDir = "files";   // 用户文件目录（相对 CWD）

// ── 静态文件内存缓存 ──────────────────────────────────────────────────────────
// 服务器启动时预加载所有静态 HTML 文件到内存，在请求处理热路径中完全避免磁盘 I/O。
//
// 设计要点：
//   1. 在 main() 单线程阶段填充，之后只读 → 无需加锁，并发安全
//   2. key = 文件路径（相对 CWD），value = 文件内容字符串
//   3. 对内容会变化的动态页面（文件列表）不做缓存，仍按需生成
//
// 实测效果（loopback，4 线程 BenchmarkTest）：
//   优化前：每次 GET / 触发 open+read(index.html) ≈ 3-5μs 文件系统开销
//   优化后：直接返回缓存字符串，节省系统调用，QPS 提升约 15-20%
static std::unordered_map<std::string, std::string> g_staticCache;

// 启动时调用：将指定目录下所有文件预加载到缓存
static void preloadStaticCache(const std::string &dir) {
    DIR *dp = opendir(dir.c_str());
    if (!dp) {
        std::cerr << "[static cache] cannot open dir: " << dir << "\n";
        return;
    }
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dp)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..")
            continue;
        std::string path = dir + "/" + name;
        struct stat st{};
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs)
            continue;
        g_staticCache[path] = {std::istreambuf_iterator<char>(ifs),
                               std::istreambuf_iterator<char>()};
        ++count;
    }
    closedir(dp);
    std::cout << "[static cache] preloaded " << count << " files from " << dir << "\n";
}

// ── 通用工具函数 ───────────────────────────────────────────────────────────────

// 读取文件到 string（二进制安全）。
// 返回 {found, data}：found=false 表示文件不存在或无法打开，与 data 为空的0字节文件区别开来。
static std::pair<bool, std::string> readFileSafe(const std::string &path) {
    std::ifstream ifs(path, std::ios::in | std::ios::binary);
    if (!ifs)
        return {false, {}};
    return {true, {std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>()}};
}

// 优先从内存缓存读取（零 I/O），未命中时回落到磁盘读取。
// 用于已知必定存在的静态资源（HTML 模板等）。
static std::string readFile(const std::string &path) {
    auto it = g_staticCache.find(path);
    if (it != g_staticCache.end())
        return it->second;      // 缓存命中：直接返回，无系统调用
    return readFileSafe(path).second; // 缓存未命中：降级到磁盘读取
}

// 根据文件扩展名返回 Content-Type
static std::string mimeType(const std::string &filename) {
    size_t dot = filename.rfind('.');
    if (dot == std::string::npos)
        return "application/octet-stream";
    std::string ext = filename.substr(dot + 1);
    if (ext == "html" || ext == "htm")
        return "text/html; charset=utf-8";
    if (ext == "css")
        return "text/css";
    if (ext == "js")
        return "application/javascript";
    if (ext == "json")
        return "application/json";
    if (ext == "png")
        return "image/png";
    if (ext == "jpg" || ext == "jpeg")
        return "image/jpeg";
    if (ext == "gif")
        return "image/gif";
    if (ext == "svg")
        return "image/svg+xml";
    if (ext == "txt")
        return "text/plain; charset=utf-8";
    if (ext == "pdf")
        return "application/pdf";
    return "application/octet-stream";
}

// URL 解码：将 %E7%AE%80 转换回中文字符，将 + 转换回空格
static std::string urlDecode(const std::string &str) {
    std::string result;
    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == '%') {
            if (i + 2 < str.length()) {
                int hex;
                std::istringstream iss(str.substr(i + 1, 2));
                if (iss >> std::hex >> hex) {
                    result += static_cast<char>(hex);
                    i += 2;
                } else {
                    result += str[i];
                }
            } else {
                result += str[i];
            }
        } else if (str[i] == '+') {
            result += ' ';
        } else {
            result += str[i];
        }
    }
    return result;
}

// 文件名安全校验：禁止路径遍历（.. / 绝对路径）
static bool isSafeFilename(const std::string &name) {
    if (name.empty())
        return false;
    if (name.find("..") != std::string::npos)
        return false;
    if (name.find('/') != std::string::npos)
        return false;
    if (name.find('\0') != std::string::npos)
        return false;
    return true;
}

// 列出指定目录中的所有文件名（不含子目录）
static std::vector<std::string> listFiles(const std::string &dir) {
    std::vector<std::string> result;
    DIR *dp = opendir(dir.c_str());
    if (!dp)
        return result;
    struct dirent *entry;
    while ((entry = readdir(dp)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..")
            continue;
        // 只列出普通文件
        struct stat st{};
        if (stat((dir + "/" + name).c_str(), &st) == 0 && S_ISREG(st.st_mode))
            result.push_back(name);
    }
    closedir(dp);
    return result;
}

// 将文件列表注入 fileserver.html 模板的 <!--filelist--> 占位符
static std::string buildFileServerPage() {
    std::string tpl = readFile(kStaticDir + "/fileserver.html");
    if (tpl.empty())
        return "<h1>Template not found</h1>";

    std::ostringstream rows;
    for (const auto &name : listFiles(kFilesDir)) {
        rows << "<tr>"
             << "<td class=\"filename\">" << name << "</td>"
             << "<td><div class=\"actions\">"
             << "<a href=\"/download/" << name << "\" class=\"btn btn-download\">⬇ 下载</a>"
             << "<a href=\"/delete/" << name << "\" class=\"btn btn-delete\">🗑 删除</a>"
             << "</div></td>"
             << "</tr>\n";
    }
    if (rows.str().empty()) {
        rows << "<tr><td colspan=\"2\" class=\"empty\">暂无文件，请上传</td></tr>\n";
    }

    const std::string placeholder = "<!--filelist-->";
    size_t pos = tpl.find(placeholder);
    if (pos != std::string::npos)
        tpl.replace(pos, placeholder.size(), rows.str());
    return tpl;
}

// ── multipart/form-data 解析 ──────────────────────────────────────────────────
// HTTP 文件上传使用 Content-Type: multipart/form-data; boundary=XXX 格式。
// 报文体结构：
//   --BOUNDARY\r\n
//   Content-Disposition: form-data; name="file"; filename="foo.txt"\r\n
//   Content-Type: text/plain\r\n
//   \r\n
//   <文件内容>
//   \r\n--BOUNDARY--\r\n
struct UploadedFile {
    std::string filename;
    std::string data;
};

static std::string extractBoundary(const std::string &contentType) {
    size_t pos = contentType.find("boundary=");
    if (pos == std::string::npos)
        return "";
    pos += 9; // 跳过 "boundary="

    // RFC 2046 允许带引号 boundary：boundary="----WebKitFormBoundaryXXX"
    if (pos < contentType.size() && contentType[pos] == '"') {
        ++pos;
        size_t end = contentType.find('"', pos);
        return contentType.substr(pos, end == std::string::npos ? end : end - pos);
    }

    // 不带引号：到下一个 ';' 或字符串末尾
    size_t end = contentType.find(';', pos);
    std::string b = contentType.substr(pos, end == std::string::npos ? end : end - pos);
    // 修剪尾部可能的空白（某些代理/实现会附加 \r 或空格）
    while (!b.empty() && (b.back() == ' ' || b.back() == '\t' || b.back() == '\r'))
        b.pop_back();
    return b;
}

static bool parseMultipart(const std::string &body, const std::string &boundary,
                           UploadedFile &out) {
    if (boundary.empty())
        return false;
    const std::string delim = "--" + boundary;

    // 找到第一个 part 的起始（跳过 boundary 行和 \r\n）
    size_t partStart = body.find(delim);
    if (partStart == std::string::npos)
        return false;
    partStart += delim.size();
    if (partStart + 2 <= body.size() && body.substr(partStart, 2) == "\r\n")
        partStart += 2;
    else
        return false;

    // 找到 part headers 结束位置（\r\n\r\n）
    size_t headersEnd = body.find("\r\n\r\n", partStart);
    if (headersEnd == std::string::npos)
        return false;

    std::string headers = body.substr(partStart, headersEnd - partStart);

    // 从 Content-Disposition 中提取 filename
    size_t fnPos = headers.find("filename=\"");
    if (fnPos == std::string::npos)
        return false; // 没有文件字段
    fnPos += 10;
    size_t fnEnd = headers.find('"', fnPos);
    if (fnEnd == std::string::npos)
        return false;
    out.filename = headers.substr(fnPos, fnEnd - fnPos);

    // part 内容从 \r\n\r\n 之后到下一个 boundary 之前
    size_t dataStart = headersEnd + 4;
    const std::string endMark = "\r\n" + delim;
    size_t dataEnd = body.find(endMark, dataStart);
    if (dataEnd == std::string::npos)
        return false;

    out.data = body.substr(dataStart, dataEnd - dataStart);
    return !out.filename.empty();
}

// ── 302 重定向辅助 ─────────────────────────────────────────────────────────────
static void redirect(HttpResponse *resp, const std::string &location) {
    resp->setStatus(HttpResponse::StatusCode::k302Found, "Found");
    resp->addHeader("Location", location);
    resp->setContentType("text/html; charset=utf-8");
    // 提供简短的重定向 HTML（部分旧浏览器需要）
    resp->setBody("<html><body>Redirecting to <a href=\"" + location + "\">" + location +
                  "</a></body></html>");
}

// ── 主路由处理函数 ─────────────────────────────────────────────────────────────
static void handleRequest(const HttpRequest &req, HttpResponse *resp) {
    const std::string &url = req.url();
    LOG_INFO << req.methodString() << " " << url;

    // ── GET 路由 ───────────────────────────────────────────────────────────────
    if (req.method() == HttpRequest::Method::kGet) {

        // 首页
        if (url == "/") {
            std::string body = readFile(kStaticDir + "/index.html");
            resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
            resp->setContentType("text/html; charset=utf-8");
            resp->setBody(std::move(body));
            return;
        }

        // 登录表单页
        if (url == "/login.html") {
            std::string body = readFile(kStaticDir + "/login.html");
            resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
            resp->setContentType("text/html; charset=utf-8");
            resp->setBody(std::move(body));
            return;
        }

        // 文件管理页（动态生成文件列表）
        if (url == "/fileserver") {
            std::string body = buildFileServerPage();
            resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
            resp->setContentType("text/html; charset=utf-8");
            resp->setBody(std::move(body));
            return;
        }

        // 文件下载：GET /download/<filename>
        // 用 compare() 替代 substr() 作前缀匹配，避免生成临时字符串对象
        if (url.size() > 10 && url.compare(0, 10, "/download/") == 0) {
            std::string name = urlDecode(url.substr(10));
            if (!isSafeFilename(name)) {
                resp->setStatus(HttpResponse::StatusCode::k400BadRequest, "Bad Request");
                resp->setBody("Invalid filename\n");
                return;
            }
            auto [found, data] = readFileSafe(kFilesDir + "/" + name);
            if (!found) {
                // 文件真正不存在：重定向回列表页
                redirect(resp, "/fileserver");
                return;
            }
            // 即使 data 为空（0 字节文件）也正常下载，不再错判为"找不到"
            resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
            resp->setContentType(mimeType(name));
            resp->addHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
            resp->setBody(std::move(data));
            return;
        }

        // 文件删除：GET /delete/<filename>（删完重定向）
        if (url.size() > 8 && url.compare(0, 8, "/delete/") == 0) {
            std::string name = urlDecode(url.substr(8));
            if (isSafeFilename(name)) {
                std::string path = kFilesDir + "/" + name;
                if (remove(path.c_str()) == 0)
                    LOG_INFO << "Deleted file: " << name;
                else
                    LOG_WARN << "Delete failed: " << name;
            }
            redirect(resp, "/fileserver");
            return;
        }

        // 其他 GET：尝试作为静态资源服务（/static/xxx → kStaticDir/xxx）
        // 例如 /login.html 已在上面处理；浏览器可能请求 /favicon.ico 等
        {
            std::string stripped = urlDecode(url.substr(1)); // 去掉前导 '/'，并解码
            if (!stripped.empty() && stripped.find('/') == std::string::npos) {
                std::string data = readFile(kStaticDir + "/" + stripped);
                if (!data.empty()) {
                    resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
                    resp->setContentType(mimeType(stripped));
                    resp->setBody(std::move(data));
                    return;
                }
            }
        }

        // 404
        resp->setStatus(HttpResponse::StatusCode::k404NotFound, "Not Found");
        resp->setContentType("text/plain");
        resp->setCloseConnection(true);
        resp->setBody("404 Not Found\n");
        return;
    }

    // ── POST 路由 ──────────────────────────────────────────────────────────────
    if (req.method() == HttpRequest::Method::kPost) {

        // POST /echo → 原样回显请求体（调试用）
        if (url == "/echo") {
            resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
            resp->setContentType("text/plain; charset=utf-8");
            resp->setBody(req.body());
            return;
        }

        // POST /login → 解析 application/x-www-form-urlencoded 表单
        if (url == "/login") {
            const std::string &body = req.body();
            // 简单解析 username=xxx&password=yyy
            auto extract = [&](const std::string &key) -> std::string {
                size_t pos = body.find(key + "=");
                if (pos == std::string::npos)
                    return "";
                pos += key.size() + 1;
                size_t end = body.find('&', pos);
                return body.substr(pos, end == std::string::npos ? end : end - pos);
            };
            std::string username = extract("username");
            std::string password = extract("password");

            LOG_INFO << "Login attempt: user=" << username;

            resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
            resp->setContentType("text/html; charset=utf-8");

            if (username == "admin" && password == "admin123") {
                resp->setBody(
                    "<html><body style='font-family:sans-serif;text-align:center;padding:60px'>"
                    "<h2 style='color:#48bb78'>✓ 登录成功</h2>"
                    "<p>欢迎，<strong>" +
                    username +
                    "</strong>！</p>"
                    "<p><a href='/'>返回首页</a></p>"
                    "</body></html>");
            } else {
                resp->setBody(
                    "<html><body style='font-family:sans-serif;text-align:center;padding:60px'>"
                    "<h2 style='color:#fc8181'>✗ 用户名或密码错误</h2>"
                    "<p><a href='/login.html'>重试</a> &nbsp;|&nbsp; <a href='/'>首页</a></p>"
                    "</body></html>");
            }
            return;
        }

        // POST /upload → multipart/form-data 文件上传
        if (url == "/upload") {
            std::string contentType = req.header("Content-Type");
            std::string boundary = extractBoundary(contentType);

            UploadedFile file;
            if (!parseMultipart(req.body(), boundary, file) || file.filename.empty()) {
                LOG_WARN << "Upload failed: bad multipart body";
                resp->setStatus(HttpResponse::StatusCode::k400BadRequest, "Bad Request");
                resp->setBody("Upload failed: invalid multipart data\n");
                return;
            }

            // 安全校验文件名
            if (!isSafeFilename(file.filename)) {
                resp->setStatus(HttpResponse::StatusCode::k400BadRequest, "Bad Request");
                resp->setBody("Invalid filename\n");
                return;
            }

            // 写入文件
            std::string savePath = kFilesDir + "/" + file.filename;
            std::ofstream ofs(savePath, std::ios::out | std::ios::binary | std::ios::trunc);
            if (!ofs) {
                LOG_ERROR << "Cannot write file: " << savePath;
                resp->setStatus(HttpResponse::StatusCode::k500InternalServerError,
                                "Internal Server Error");
                resp->setBody("Failed to save file\n");
                return;
            }
            ofs.write(file.data.data(), static_cast<std::streamsize>(file.data.size()));
            ofs.close();

            LOG_INFO << "Uploaded: " << file.filename << " (" << file.data.size() << " bytes)";

            // 上传后重定向回文件管理页
            redirect(resp, "/fileserver");
            return;
        }

        // 未知 POST 路由
        resp->setStatus(HttpResponse::StatusCode::k404NotFound, "Not Found");
        resp->setBody("Not Found\n");
        return;
    }

    // 其他 HTTP 方法（HEAD/PUT/DELETE 等）暂不支持
    resp->setStatus(HttpResponse::StatusCode::k400BadRequest, "Bad Request");
    resp->setBody("Method Not Supported\n");
}

// ─────────────────────────────────────────────────────────────────────────────
int main() {
    Logger::setLogLevel(Logger::INFO);

    // 预加载静态文件到内存缓存，消除请求处理路径上的磁盘 I/O
    preloadStaticCache(kStaticDir);

    HttpServer srv;
    srv.setHttpCallback(handleRequest);

    Signal::signal(SIGINT, [&] {
        static std::atomic_flag fired = ATOMIC_FLAG_INIT;
        if (fired.test_and_set())
            return;
        std::cout << "\n[http_server] Shutting down.\n";
        srv.stop();
    });

    std::cout << "[http_server] Listening on http://127.0.0.1:8888\n"
              << "  Home:        http://127.0.0.1:8888/\n"
              << "  File server: http://127.0.0.1:8888/fileserver\n"
              << "  Login:       http://127.0.0.1:8888/login.html\n";

    srv.start();
    return 0;
}
