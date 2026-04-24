#include <http/HttpRequest.h>
#include <http/HttpResponse.h>
#include <http/HttpServer.h>
#include <http/WebSocket.h>
#include <log/AsyncLogging.h>
#include <log/Logger.h>
#include <net/SignalHandler.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#ifdef HAS_ZLIB
#include <zlib.h>
#endif

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
        LOG_ERROR << "[static cache] cannot open dir: " << dir;
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
    LOG_INFO << "[static cache] preloaded " << count << " files from " << dir;
}

// ── 通用工具函数 ───────────────────────────────────────────────────────────────

// 读取文件到 string（二进制安全，mmap 快路径）。
// 返回 {found, data}：found=false 表示文件不存在或无法打开，与 data 为空的0字节文件区别开来。
static std::pair<bool, std::string> readFileSafe(const std::string &path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd == -1)
        return {false, {}};

    struct stat st{};
    if (::fstat(fd, &st) == -1 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return {false, {}};
    }

    if (st.st_size == 0) {
        ::close(fd);
        return {true, {}};
    }

    void *mapped = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        // mmap 不可用时退回到流式读取。
        ::close(fd);
        std::ifstream ifs(path, std::ios::in | std::ios::binary);
        if (!ifs)
            return {false, {}};
        return {true, {std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>()}};
    }

    std::string data(static_cast<const char *>(mapped), static_cast<size_t>(st.st_size));
    ::munmap(mapped, static_cast<size_t>(st.st_size));
    ::close(fd);
    return {true, std::move(data)};
}

static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static std::string toHttpDate(time_t t) {
    char buf[128];
    struct tm gmt{};
    gmtime_r(&t, &gmt);
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &gmt);
    return buf;
}

static bool parseRangeHeader(const std::string &rangeHeader, size_t fileSize, size_t *start,
                             size_t *length) {
    if (rangeHeader.empty() || rangeHeader.compare(0, 6, "bytes=") != 0)
        return false;

    std::string spec = rangeHeader.substr(6);
    size_t dash = spec.find('-');
    if (dash == std::string::npos)
        return false;

    std::string left = spec.substr(0, dash);
    std::string right = spec.substr(dash + 1);

    if (fileSize == 0)
        return false;

    if (left.empty()) {
        // bytes=-N（后缀范围）
        long suffix = std::strtol(right.c_str(), nullptr, 10);
        if (suffix <= 0)
            return false;
        size_t n = static_cast<size_t>(suffix);
        if (n > fileSize)
            n = fileSize;
        *start = fileSize - n;
        *length = n;
        return true;
    }

    long begin = std::strtol(left.c_str(), nullptr, 10);
    if (begin < 0 || static_cast<size_t>(begin) >= fileSize)
        return false;

    size_t s = static_cast<size_t>(begin);
    size_t e = fileSize - 1;
    if (!right.empty()) {
        long end = std::strtol(right.c_str(), nullptr, 10);
        if (end < begin)
            return false;
        e = static_cast<size_t>(end);
        if (e >= fileSize)
            e = fileSize - 1;
    }

    *start = s;
    *length = e - s + 1;
    return true;
}

static std::string buildWeakEtag(const struct stat &st) {
    return "W/\"" + std::to_string(static_cast<unsigned long long>(st.st_size)) + "-" +
           std::to_string(static_cast<unsigned long long>(st.st_mtime)) + "\"";
}

static bool serveFileWithNegotiation(const HttpRequest &req, HttpResponse *resp,
                                     const std::string &path, const std::string &downloadName,
                                     const std::string &cacheControl, bool enableRange) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
        return false;

    const std::string etag = buildWeakEtag(st);
    const std::string lastModified = toHttpDate(st.st_mtime);
    const size_t fileSize = static_cast<size_t>(st.st_size);

    resp->addHeader("ETag", etag);
    resp->addHeader("Last-Modified", lastModified);
    resp->addHeader("Cache-Control", cacheControl);
    resp->addHeader("Accept-Ranges", "bytes");
    if (!downloadName.empty()) {
        resp->addHeader("Content-Disposition", "attachment; filename=\"" + downloadName + "\"");
        resp->setContentTypeByFilename(downloadName);
    } else {
        size_t slash = path.find_last_of('/');
        resp->setContentTypeByFilename(slash == std::string::npos ? path : path.substr(slash + 1));
    }

    if (req.header("If-None-Match") == etag) {
        resp->setStatus(HttpResponse::StatusCode::k304NotModified, "Not Modified");
        resp->setBody("");
        return true;
    }

    size_t start = 0;
    size_t length = fileSize;
    const std::string rangeHeader = req.header("Range");
    if (enableRange && !rangeHeader.empty()) {
        if (!parseRangeHeader(rangeHeader, fileSize, &start, &length)) {
            resp->setStatus(HttpResponse::StatusCode::k416RangeNotSatisfiable,
                            "Range Not Satisfiable");
            resp->addHeader("Content-Range", "bytes */" + std::to_string(fileSize));
            resp->setBody("\n");
            return true;
        }
        resp->setStatus(HttpResponse::StatusCode::k206PartialContent, "Partial Content");
        resp->addHeader("Content-Range", "bytes " + std::to_string(start) + "-" +
                                             std::to_string(start + length - 1) + "/" +
                                             std::to_string(fileSize));
    } else {
        resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
    }

    resp->setSendFile(path, start, length);
    return true;
}

// 优先从内存缓存读取（零 I/O），未命中时回落到磁盘读取。
// 用于已知必定存在的静态资源（HTML 模板等）。
static std::string readFile(const std::string &path) {
    auto it = g_staticCache.find(path);
    if (it != g_staticCache.end())
        return it->second;            // 缓存命中：直接返回，无系统调用
    return readFileSafe(path).second; // 缓存未命中：降级到磁盘读取
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

static bool isCompressibleContentType(const std::string &contentType) {
    const std::string ct = toLower(contentType);
    return ct.find("text/") != std::string::npos ||
           ct.find("application/json") != std::string::npos ||
           ct.find("application/javascript") != std::string::npos ||
           ct.find("application/xml") != std::string::npos;
}

#ifdef HAS_ZLIB
static bool gzipCompress(const std::string &input, std::string *output) {
    if (!output)
        return false;

    z_stream zs{};
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) !=
        Z_OK) {
        return false;
    }

    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
    zs.avail_in = static_cast<uInt>(input.size());

    std::string out;
    out.resize(compressBound(static_cast<uLong>(input.size())));
    zs.next_out = reinterpret_cast<Bytef *>(out.data());
    zs.avail_out = static_cast<uInt>(out.size());

    int rc = deflate(&zs, Z_FINISH);
    if (rc != Z_STREAM_END) {
        deflateEnd(&zs);
        return false;
    }

    out.resize(zs.total_out);
    deflateEnd(&zs);
    *output = std::move(out);
    return true;
}
#endif

// ── 主路由处理函数 ─────────────────────────────────────────────────────────────
static void handleRequest(const HttpRequest &req, HttpResponse *resp) {
    const std::string &url = req.url();
    LOG_INFO << req.methodString() << " " << url;

    if (req.method() == HttpRequest::Method::kOptions) {
        resp->setStatus(HttpResponse::StatusCode::k204NoContent, "No Content");
        resp->setBody("");
        return;
    }

    // ── GET 路由 ───────────────────────────────────────────────────────────────
    if (req.method() == HttpRequest::Method::kGet || req.method() == HttpRequest::Method::kHead) {

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

        // WebSocket 演示页面（页面内 JS 会发起 ws://host/ws/echo 升级请求）
        if (url == "/ws.html" || url == "/ws") {
            std::string body = readFile(kStaticDir + "/ws.html");
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
            std::string name = HttpRequest::urlDecode(url.substr(10));
            if (!isSafeFilename(name)) {
                resp->setStatus(HttpResponse::StatusCode::k400BadRequest, "Bad Request");
                resp->setBody("Invalid filename\n");
                return;
            }
            const std::string path = kFilesDir + "/" + name;
            if (!serveFileWithNegotiation(req, resp, path, name,
                                          "private, max-age=0, must-revalidate", true)) {
                // 文件真正不存在：重定向回列表页
                resp->setRedirect("/fileserver");
                return;
            }
            return;
        }

        // 文件删除：GET /delete/<filename>（删完重定向）
        if (url.size() > 8 && url.compare(0, 8, "/delete/") == 0) {
            std::string name = HttpRequest::urlDecode(url.substr(8));
            if (isSafeFilename(name)) {
                std::string path = kFilesDir + "/" + name;
                if (remove(path.c_str()) == 0)
                    LOG_INFO << "Deleted file: " << name;
                else
                    LOG_WARN << "Delete failed: " << name;
            }
            resp->setRedirect("/fileserver");
            return;
        }

        // 其他 GET：尝试作为静态资源服务（/static/xxx → kStaticDir/xxx）
        // 例如 /login.html 已在上面处理；浏览器可能请求 /favicon.ico 等
        {
            std::string stripped = HttpRequest::urlDecode(url.substr(1)); // 去掉前导 '/'，并解码
            if (!stripped.empty() && stripped.find("..") == std::string::npos) {
                std::string path = kStaticDir + "/" + stripped;
                if (serveFileWithNegotiation(req, resp, path, "", "public, max-age=60", true)) {
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

            HttpRequest::MultipartFile file;
            if (!req.parseMultipart(file) || file.filename.empty()) {
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
            resp->setRedirect("/fileserver");
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
std::unique_ptr<AsyncLogging> g_asyncLog;

void asyncOutputLog(const char *data, int len) {
    // 追加写入异步日志系统（写内存缓冲区，无锁或极短临界区，不阻塞业务线程）
    g_asyncLog->append(data, len);
}

static std::string envOr(const char *key, const std::string &fallback) {
    const char *v = std::getenv(key);
    if (!v || v[0] == '\0')
        return fallback;
    return v;
}

static int envIntOr(const char *key, int fallback) {
    const char *v = std::getenv(key);
    if (!v || v[0] == '\0')
        return fallback;
    errno = 0;
    char *end = nullptr;
    long parsed = std::strtol(v, &end, 10);
    if (errno != 0 || end == v || *end != '\0')
        return fallback;
    if (parsed > std::numeric_limits<int>::max() || parsed < std::numeric_limits<int>::min())
        return fallback;
    return static_cast<int>(parsed);
}

static size_t envSizeOr(const char *key, size_t fallback) {
    const int parsed = envIntOr(key, static_cast<int>(fallback));
    if (parsed <= 0)
        return fallback;
    return static_cast<size_t>(parsed);
}

static uint16_t envPortOr(const char *key, uint16_t fallback) {
    int p = envIntOr(key, static_cast<int>(fallback));
    if (p <= 0 || p > 65535)
        return fallback;
    return static_cast<uint16_t>(p);
}

static bool envBoolOr(const char *key, bool fallback) {
    const char *v = std::getenv(key);
    if (!v || v[0] == '\0')
        return fallback;
    std::string s(v);
    if (s == "1" || s == "true" || s == "TRUE" || s == "on" || s == "ON")
        return true;
    if (s == "0" || s == "false" || s == "FALSE" || s == "off" || s == "OFF")
        return false;
    return fallback;
}

static double envDoubleOr(const char *key, double fallback) {
    const char *v = std::getenv(key);
    if (!v || v[0] == '\0')
        return fallback;
    errno = 0;
    char *end = nullptr;
    double parsed = std::strtod(v, &end);
    if (errno != 0 || end == v || *end != '\0')
        return fallback;
    if (parsed <= 0.0)
        return fallback;
    return parsed;
}

int main() {
    // 确保存在日志归档目录（以处理“日志文本文档默认散落于build文件夹下”的问题）
    mkdir("log", 0755);

    // 实例化并启动异步日志后端线程
    g_asyncLog = std::make_unique<AsyncLogging>("log/http_server", 50 * 1024 * 1024, 3);
    g_asyncLog->start();

    // 注册到全局 Logger 输出管道，接管程序内所有 LOG_INFO、LOG_ERROR 等宏产生的内容
    Logger::setOutput(asyncOutputLog);
    Logger::setLogLevel(Logger::INFO);

    // 预加载静态文件到内存缓存，消除请求处理路径上的磁盘 I/O
    preloadStaticCache(kStaticDir);

    HttpServer::Options options;
    options.tcp.listenIp = envOr("MYCPPSERVER_BIND_IP", "127.0.0.1");
    options.tcp.listenPort = envPortOr("MYCPPSERVER_BIND_PORT", 8888);
    options.tcp.ioThreads = envIntOr("MYCPPSERVER_IO_THREADS", 0);
    options.tcp.maxConnections = envSizeOr("MYCPPSERVER_MAX_CONNECTIONS", 10000);
    options.tcp.tls.enabled = envBoolOr("MYCPPSERVER_TLS_ENABLE", false);
    options.tcp.tls.certFile = envOr("MYCPPSERVER_TLS_CERT_FILE", "");
    options.tcp.tls.keyFile = envOr("MYCPPSERVER_TLS_KEY_FILE", "");
    options.autoClose = envBoolOr("MYCPPSERVER_AUTO_CLOSE", false);
    options.idleTimeoutSec = envDoubleOr("MYCPPSERVER_IDLE_TIMEOUT_SEC", 60.0);
    options.requestTimeoutSec = envDoubleOr("MYCPPSERVER_REQUEST_TIMEOUT_SEC", 15.0);
    options.limits.maxRequestLineBytes = envIntOr("MYCPPSERVER_MAX_REQUEST_LINE_BYTES", 8 * 1024);
    options.limits.maxHeaderBytes = envIntOr("MYCPPSERVER_MAX_HEADER_BYTES", 32 * 1024);
    options.limits.maxBodyBytes = envIntOr("MYCPPSERVER_MAX_BODY_BYTES", 10 * 1024 * 1024);

    HttpServer srv(options);

    // CORS + 预检请求支持。
    srv.use([](const HttpRequest &req, HttpResponse *resp, const HttpServer::MiddlewareNext &next) {
        resp->addHeader("Access-Control-Allow-Origin", "*");
        resp->addHeader("Access-Control-Allow-Methods", "GET,POST,PUT,DELETE,OPTIONS,HEAD");
        resp->addHeader("Access-Control-Allow-Headers",
                        "Content-Type,Authorization,If-None-Match,If-Modified-Since,Range");
        resp->addHeader("Access-Control-Max-Age", "600");
        if (req.method() == HttpRequest::Method::kOptions) {
            resp->setStatus(HttpResponse::StatusCode::k204NoContent, "No Content");
            resp->setBody("");
            return;
        }
        next();
    });

    // 通用安全响应头。
    srv.use([](const HttpRequest &, HttpResponse *resp, const HttpServer::MiddlewareNext &next) {
        next();
        resp->addHeader("X-Content-Type-Options", "nosniff");
        resp->addHeader("Referrer-Policy", "no-referrer");
    });

    // gzip 压缩（可选，需编译时存在 zlib）。
    srv.use([](const HttpRequest &req, HttpResponse *resp, const HttpServer::MiddlewareNext &next) {
        next();

        if (resp->hasSendFile())
            return;
        if (resp->statusCode() != HttpResponse::StatusCode::k200OK)
            return;

        const std::string acceptEncoding = toLower(req.header("Accept-Encoding"));
        if (acceptEncoding.find("gzip") == std::string::npos)
            return;

        const std::string contentType = resp->header("Content-Type");
        if (!isCompressibleContentType(contentType))
            return;
        if (resp->body().size() < 64)
            return;

#ifdef HAS_ZLIB
        std::string compressed;
        if (!gzipCompress(resp->body(), &compressed))
            return;
        if (compressed.size() >= resp->body().size())
            return;

        resp->setBody(std::move(compressed));
        resp->addHeader("Content-Encoding", "gzip");
        resp->addHeader("Vary", "Accept-Encoding");
#endif
    });

    srv.setHttpCallback(handleRequest);

    // ── WebSocket 路由 (day31) ────────────────────────────────────────────────
    // ws://<host>:<port>/ws/echo —— 把客户端发来的文本/二进制原样回写
    // 在浏览器中打开 /ws.html 可看到完整的交互演示页面
    WebSocketHandler echoHandler;
    echoHandler.onOpen = [](WebSocketConnection &c) {
        LOG_INFO << "[ws/echo] open";
        c.sendText("welcome to Airi-Cpp-Server-Lib WebSocket echo");
    };
    echoHandler.onMessage = [](WebSocketConnection &c, const std::string &msg, bool isBinary) {
        LOG_INFO << "[ws/echo] recv " << (isBinary ? "<binary> " : "") << msg.size() << "B";
        if (msg == "[PING]") {
            // 模拟 ping/pong：浏览器 API 不允许直接发控制帧，这里用文本兼容
            c.sendText("[PONG] " + std::to_string(msg.size()));
        } else if (isBinary) {
            c.sendBinary(msg);
        } else {
            c.sendText(msg);
        }
    };
    echoHandler.onClose = [](WebSocketConnection &, uint16_t code, const std::string &reason) {
        LOG_INFO << "[ws/echo] close code=" << code << " reason=" << reason;
    };
    echoHandler.onPing = [](WebSocketConnection &c, const std::string &payload) {
        c.sendPong(payload); // 自动回 pong
    };
    echoHandler.onPong = [](WebSocketConnection &, const std::string &) {};
    srv.addWebSocketRoute("/ws/echo", echoHandler);

    Signal::signal(SIGINT, [&] {
        static std::atomic_flag fired = ATOMIC_FLAG_INIT;
        if (fired.test_and_set())
            return;
        std::cout << "\n[http_server] Shutting down.\n";
        srv.stop();
        LOG_INFO << "[http_server] Shutting down.";
        srv.stop();
        g_asyncLog->stop(); // 安全停掉后台日志线程以保证剩余数据被刷写
    });

    LOG_INFO << "[http_server] config: listen=" << options.tcp.listenIp << ":"
             << options.tcp.listenPort << " ioThreads=" << options.tcp.ioThreads
             << " maxConnections=" << options.tcp.maxConnections
             << " autoClose=" << (options.autoClose ? "true" : "false")
             << " idleTimeoutSec=" << options.idleTimeoutSec;
    LOG_INFO << "[http_server] Listening on http://" << options.tcp.listenIp << ":"
             << options.tcp.listenPort;
    LOG_INFO << "  Home:        http://" << options.tcp.listenIp << ":" << options.tcp.listenPort
             << "/";
    LOG_INFO << "  File server: http://" << options.tcp.listenIp << ":" << options.tcp.listenPort
             << "/fileserver";
    LOG_INFO << "  Login:       http://" << options.tcp.listenIp << ":" << options.tcp.listenPort
             << "/login.html";
    LOG_INFO << "  WebSocket:   http://" << options.tcp.listenIp << ":" << options.tcp.listenPort
             << "/ws.html  (echo at ws:// .../ws/echo)";

    srv.start();
    return 0;
}
