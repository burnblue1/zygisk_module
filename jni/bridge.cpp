#include "bridge.h"
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <algorithm>

#define TAG "Bridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// ============================================
// 单例
// ============================================
BridgeServer& BridgeServer::getInstance() {
    static BridgeServer instance;
    return instance;
}

// ============================================
// JSON 工具
// ============================================
std::string BridgeServer::escapeJSON(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\r': r += "\\r"; break;
            case '\t': r += "\\t"; break;
            default:   r += c;
        }
    }
    return r;
}

std::string BridgeServer::getJSONString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) {
        search = "\"" + key + "\":";
        pos = json.find(search);
        if (pos == std::string::npos) return "";
        pos += search.length();
        while (pos < json.size() && json[pos] == ' ') pos++;
        size_t end = pos;
        while (end < json.size() && json[end] != ',' && json[end] != '}' && json[end] != ' ') end++;
        return json.substr(pos, end - pos);
    }
    pos += search.length();
    size_t end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

// ============================================
// 启动/停止
// ============================================
void BridgeServer::start(int port) {
    if (running_) return;
    
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        LOGE("Failed to create socket");
        return;
    }
    
    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(server_fd_, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
    
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(server_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(port);
    
    if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOGE("Failed to bind port %d: %s", port, strerror(errno));
        close(server_fd_);
        server_fd_ = -1;
        return;
    }
    
    if (listen(server_fd_, 8) < 0) {
        LOGE("Failed to listen: %s", strerror(errno));
        close(server_fd_);
        server_fd_ = -1;
        return;
    }
    
    running_ = true;
    server_thread_ = std::thread(&BridgeServer::serverLoop, this);
    
    LOGI("Bridge server started on 127.0.0.1:%d", port);
}

void BridgeServer::stop() {
    running_ = false;
    
    if (server_fd_ >= 0) {
        shutdown(server_fd_, SHUT_RDWR);
        close(server_fd_);
        server_fd_ = -1;
    }
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (int fd : client_fds_) {
            shutdown(fd, SHUT_RDWR);
            close(fd);
        }
        client_fds_.clear();
    }
    
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
    
    LOGI("Bridge server stopped");
}

// ============================================
// 主循环
// ============================================
void BridgeServer::serverLoop() {
    while (running_) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_fd_, &readfds);
        int maxfd = server_fd_;
        
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (int fd : client_fds_) {
                FD_SET(fd, &readfds);
                if (fd > maxfd) maxfd = fd;
            }
        }
        
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        int activity = select(maxfd + 1, &readfds, nullptr, nullptr, &tv);
        if (activity < 0) {
            if (!running_) break;
            continue;
        }
        
        if (FD_ISSET(server_fd_, &readfds)) {
            int client = accept(server_fd_, nullptr, nullptr);
            if (client >= 0) {
                LOGI("New client: %d", client);
                std::lock_guard<std::mutex> lock(mutex_);
                client_fds_.push_back(client);
                std::string welcome = "{\"type\":\"log\",\"level\":\"INFO\",\"msg\":\"Bridge Server ready\"}\n";
                write(client, welcome.c_str(), welcome.length());
            }
        }
        
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = client_fds_.begin();
        while (it != client_fds_.end()) {
            int fd = *it;
            
            if (FD_ISSET(fd, &readfds)) {
                char buf[65536];
                int n = read(fd, buf, sizeof(buf) - 1);
                
                if (n <= 0) {
                    LOGI("Client disconnected: %d", fd);
                    close(fd);
                    it = client_fds_.erase(it);
                    continue;
                }
                
                buf[n] = '\0';
                
                if (strncmp(buf, "GET ", 4) == 0 || strncmp(buf, "POST ", 5) == 0) {
                    handleClient(fd);
                    close(fd);
                    it = client_fds_.erase(it);
                    continue;
                }
                
                std::string data(buf);
                size_t start = 0;
                size_t end;
                while ((end = data.find('\n', start)) != std::string::npos) {
                    std::string line = data.substr(start, end - start);
                    start = end + 1;
                    
                    if (line.empty()) continue;
                    
                    std::string response = processCommand(line);
                    if (!response.empty()) {
                        response += "\n";
                        write(fd, response.c_str(), response.length());
                    }
                }
            }
            
            ++it;
        }
    }
}

// ============================================
// HTTP 处理
// ============================================
void BridgeServer::handleClient(int client_fd) {
    char buf[65536];
    int n = read(client_fd, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = '\0';
    
    std::string request(buf);
    
    std::string method, path;
    std::istringstream iss(request);
    iss >> method >> path;
    
    if (path == "/" || path == "/index.html") {
        const char* html = R"HTML(HTTP/1.1 200 OK
Content-Type: text/html; charset=utf-8
Connection: close

<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0,user-scalable=no">
<title>游戏助手</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{background:#0a0a0f;color:#e0e0e0;font-family:'SF Mono',Consolas,monospace;font-size:13px;padding:10px;min-height:100vh}
.header{text-align:center;padding:12px 0;border-bottom:1px solid #1e1e2e;margin-bottom:10px}
.header h1{color:#00d4ff;font-size:18px;font-weight:700}
.header span{color:#666;font-size:11px}
.card{background:#12121a;border:1px solid #1e1e2e;border-radius:10px;padding:12px;margin:8px 0}
.card-title{color:#00d4ff;font-size:13px;font-weight:600;margin-bottom:10px;display:flex;align-items:center;gap:6px}
.card-title .status{font-size:10px;color:#555;margin-left:auto}
.card-title .status.on{color:#00ff88}
.btns{display:flex;flex-wrap:wrap;gap:6px}
button{padding:6px 14px;border:1px solid #2a2a3a;border-radius:6px;background:#0a0a0f;color:#ccc;font-family:inherit;font-size:11px;cursor:pointer;transition:all .15s;white-space:nowrap}
button:active{transform:scale(.95)}
button.on{border-color:#00ff88;color:#00ff88;background:rgba(0,255,136,.05)}
button.primary{border-color:#00d4ff;color:#00d4ff}
button.danger{border-color:#ff4455;color:#ff4455}
button.warn{border-color:#ffaa00;color:#ffaa00}
button:disabled{opacity:.35}
input[type=number]{background:#0a0a0f;border:1px solid #2a2a3a;color:#e0e0e0;padding:6px 10px;border-radius:6px;font-family:inherit;font-size:12px;width:70px;text-align:center;outline:none}
input:focus{border-color:#00d4ff}
.log-area{background:#06060a;border:1px solid #1e1e2e;border-radius:8px;padding:10px;height:250px;overflow-y:auto;font-size:10px;line-height:1.6;white-space:pre-wrap;word-break:break-all}
.log-time{color:#555}
.flex{display:flex;align-items:center;gap:8px}
</style>
</head>
<body>

<div class="header">
  <h1>🎮 游戏辅助控制台</h1>
  <span>Zygisk Injection · Port 27042</span>
</div>

<div class="card">
  <div class="card-title">⚔️ 冰原巨兽 <span class="status" id="st-battle">未加载</span></div>
  <div class="btns">
    <button onclick="loadMod('auto_battle',this)">📂 加载</button>
    <button onclick="cmd('initbattle')">🔧 初始化</button>
    <button class="on" onclick="cmd('startbattle')">▶ 启动</button>
    <button class="danger" onclick="cmd('stopbattle')">■ 停止</button>
  </div>
</div>

<div class="card">
  <div class="card-title">🐻 巨熊行动 <span class="status" id="st-bear">未加载</span></div>
  <div class="btns">
    <button onclick="loadMod('auto_bear',this)">📂 加载</button>
    <button class="warn" onclick="cmd('createmarch')">🚩 开集结</button>
    <button class="on" onclick="cmd('startauto')">▶ 启动</button>
    <button class="danger" onclick="cmd('stopauto')">■ 停止</button>
  </div>
</div>

<div class="card">
  <div class="card-title">🏰 自动王城 <span class="status" id="st-attack">未加载</span></div>
  <div class="btns">
    <button onclick="loadMod('auto_attack',this)">📂 加载</button>
    <button onclick="cmd('attackinit')">🔧 初始化</button>
    <button class="on" onclick="cmd('startautoattack')">▶ 启动</button>
    <button class="danger" onclick="cmd('stopautoattack')">■ 停止</button>
  </div>
</div>

<div class="card">
  <div class="card-title">💊 自动治疗 <span class="status" id="st-heal">未加载</span></div>
  <div class="btns flex">
    <button onclick="loadMod('auto_heal',this)">📂 加载</button>
    <button onclick="cmd('healinit')">🔧 初始化</button>
    <input type="number" id="heal-count" value="50" min="1" max="9999" style="width:55px">
    <button class="on" onclick="cmd('startautoheal',document.getElementById('heal-count').value)">▶ 启动</button>
    <button class="danger" onclick="cmd('stopautoheal')">■ 停止</button>
  </div>
</div>

<div class="card">
  <div class="card-title">👹 召唤Boss <span class="status" id="st-hunter">未加载</span></div>
  <div class="btns">
    <button onclick="loadMod('auto_hunter',this)">📂 加载</button>
    <button class="on" onclick="cmd('starthunter')">▶ 启动</button>
    <button class="danger" onclick="cmd('stophunter')">■ 停止</button>
  </div>
</div>

<div class="card">
  <div class="card-title">🔧 核心模块</div>
  <div class="btns">
    <button onclick="loadMod('msg_dispatcher',this)">📨 消息分发</button>
    <button onclick="loadMod('formation_manager',this)">⚔️ 编队管理</button>
  </div>
</div>

<div class="card">
  <div class="card-title">📋 运行日志</div>
  <div class="log-area" id="log"></div>
  <div class="btns" style="margin-top:8px">
    <button onclick="cmd('ping')">🏓 Ping</button>
    <button onclick="cmd('get_status')">📊 状态</button>
    <button onclick="document.getElementById('log').innerHTML=''">🗑️ 清屏</button>
  </div>
</div>

<script>
var loaded={};
function log(msg,level){
  level=level||'INFO';
  var el=document.getElementById('log');
  var t=new Date().toLocaleTimeString();
  el.innerHTML+='<div>['+t+'] '+msg+'</div>';
  el.scrollTop=el.scrollHeight;
}
async function cmd(name,params){
  try{
    var r=await fetch('/api/'+name,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({params:params||''})});
    var j=await r.json();
    log('OK '+name+': '+(j.data||'OK'));
  }catch(e){
    log('ERR '+name+': '+e.message,'ERROR');
  }
}
async function loadMod(name,btn){
  if(loaded[name]){
    await cmd('unload_module',name);
    loaded[name]=false;
    btn.textContent='📂 加载';
    btn.classList.remove('danger');
    return;
  }
  btn.textContent='...';
  btn.disabled=true;
  await cmd('load_module',name);
  loaded[name]=true;
  btn.textContent='🗑️ 卸载';
  btn.classList.add('danger');
  btn.disabled=false;
}
log('Ready');
</script>
</body>
</html>
)HTML";
        write(client_fd, html, strlen(html));
    }
    else if (path.find("/api/") == 0) {
        std::string api_name = path.substr(5);
        
        std::string body;
        size_t body_pos = request.find("\r\n\r\n");
        if (body_pos != std::string::npos) {
            body = request.substr(body_pos + 4);
        }
        
        std::string params = getJSONString(body, "params");
        std::string result = processCommand("{\"cmd\":\"" + api_name + "\",\"params\":\"" + escapeJSON(params) + "\"}");
        
        std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n" + result;
        write(client_fd, response.c_str(), response.length());
    }
    else {
        const char* notfound = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nNot Found";
        write(client_fd, notfound, strlen(notfound));
    }
}

// ============================================
// 命令处理
// ============================================
std::string BridgeServer::processCommand(const std::string& raw_json) {
    std::string cmd = getJSONString(raw_json, "cmd");
    std::string params = getJSONString(raw_json, "params");
    
    if (cmd.empty()) {
        return "{\"status\":\"error\",\"data\":\"empty command\"}";
    }
    
    auto it = commands_.find(cmd);
    if (it != commands_.end()) {
        try {
            std::string result = it->second(params);
            std::ostringstream ss;
            ss << "{\"status\":\"ok\",\"data\":\"" << escapeJSON(result) << "\"}";
            return ss.str();
        } catch (const std::exception& e) {
            std::ostringstream ss;
            ss << "{\"status\":\"error\",\"data\":\"" << escapeJSON(e.what()) << "\"}";
            return ss.str();
        }
    }
    
    return "{\"status\":\"error\",\"data\":\"Unknown command: " + escapeJSON(cmd) + "\"}";
}

void BridgeServer::registerCommand(const std::string& cmd, CommandCallback cb) {
    commands_[cmd] = cb;
}

void BridgeServer::sendLog(const std::string& level, const std::string& msg) {
    std::ostringstream ss;
    ss << "{\"type\":\"log\",\"level\":\"" << level << "\",\"msg\":\"" << escapeJSON(msg) << "\"}\n";
    broadcast(ss.str());
}

void BridgeServer::broadcast(const std::string& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = client_fds_.begin();
    while (it != client_fds_.end()) {
        int ret = write(*it, data.c_str(), data.length());
        if (ret < 0) {
            close(*it);
            it = client_fds_.erase(it);
        } else {
            ++it;
        }
    }
}

void BridgeServer::setLuaExecutor(std::function<bool(const std::string&)> executor) {
    lua_executor_ = executor;
}

bool BridgeServer::execLua(const std::string& code) {
    if (lua_executor_) {
        return lua_executor_(code);
    }
    sendLog("WARN", "Lua executor not set");
    return false;
}

// ============================================
// 全局
// ============================================
void start_bridge_server(int port) {
    BridgeServer::getInstance().start(port);
}

static std::vector<std::string> loaded_modules;
static std::mutex modules_mutex;

void register_all_commands() {
    auto& bridge = BridgeServer::getInstance();
    
    bridge.registerCommand("ping", [](const std::string& params) -> std::string {
        return "pong";
    });
    
    bridge.registerCommand("get_status", [](const std::string& params) -> std::string {
        std::lock_guard<std::mutex> lock(modules_mutex);
        std::ostringstream ss;
        ss << "{\"injected\":true,\"modules\":[";
        for (size_t i = 0; i < loaded_modules.size(); i++) {
            if (i > 0) ss << ",";
            ss << "\"" << loaded_modules[i] << "\"";
        }
        ss << "]}";
        return ss.str();
    });
    
    bridge.registerCommand("load_module", [](const std::string& params) -> std::string {
        std::lock_guard<std::mutex> lock(modules_mutex);
        if (std::find(loaded_modules.begin(), loaded_modules.end(), params) == loaded_modules.end()) {
            loaded_modules.push_back(params);
        }
        BridgeServer::getInstance().sendLog("INFO", "Module loaded: " + params);
        return "Module " + params + " loaded";
    });
    
    bridge.registerCommand("unload_module", [](const std::string& params) -> std::string {
        std::lock_guard<std::mutex> lock(modules_mutex);
        auto it = std::find(loaded_modules.begin(), loaded_modules.end(), params);
        if (it != loaded_modules.end()) {
            loaded_modules.erase(it);
        }
        BridgeServer::getInstance().sendLog("INFO", "Module unloaded: " + params);
        return "Module " + params + " unloaded";
    });
    
    bridge.registerCommand("exec_lua", [](const std::string& params) -> std::string {
        BridgeServer::getInstance().sendLog("INFO", "Exec Lua: " + params.substr(0, 80));
        bool ok = BridgeServer::getInstance().execLua(params);
        return ok ? "Lua executed" : "Lua execution failed";
    });
    
    bridge.registerCommand("initbattle", [](const std::string& params) -> std::string {
        BridgeServer::getInstance().sendLog("INFO", "Battle init");
        return "Battle initialized";
    });
    
    bridge.registerCommand("startbattle", [](const std::string& params) -> std::string {
        BridgeServer::getInstance().sendLog("INFO", "Battle started");
        return "Battle started";
    });
    
    bridge.registerCommand("stopbattle", [](const std::string& params) -> std::string {
        BridgeServer::getInstance().sendLog("INFO", "Battle stopped");
        return "Battle stopped";
    });
    
    bridge.registerCommand("searchboss", [](const std::string& params) -> std::string {
        BridgeServer::getInstance().sendLog("INFO", "Search boss level " + params);
        return "Searching boss level " + params;
    });
    
    bridge.registerCommand("startauto", [](const std::string& params) -> std::string {
        BridgeServer::getInstance().sendLog("INFO", "Bear started");
        return "Auto bear started";
    });
    
    bridge.registerCommand("stopauto", [](const std::string& params) -> std::string {
        BridgeServer::getInstance().sendLog("INFO", "Bear stopped");
        return "Auto bear stopped";
    });
    
    bridge.registerCommand("createmarch", [](const std::string& params) -> std::string {
        BridgeServer::getInstance().sendLog("INFO", "March created");
        return "March created";
    });
    
    bridge.registerCommand("addtargetuid", [](const std::string& params) -> std::string {
        BridgeServer::getInstance().sendLog("INFO", "Add target: " + params);
        return "Target added: " + params;
    });
    
    bridge.registerCommand("removetargetuid", [](const std::string& params) -> std::string {
        BridgeServer::getInstance().sendLog("INFO", "Remove target: " + params);
        return "Target removed: " + params;
    });
    
    bridge.registerCommand("gettargetuids", [](const std::string& params) -> std::string {
        return "[]";
    });
    
    bridge.registerCommand("attackinit", [](const std::string& params) -> std::string {
        BridgeServer::getInstance().sendLog("INFO", "Attack init");
        return "Attack initialized";
    });
    
    bridge.registerCommand("startautoattack", [](const std::string& params) -> std::string {
        BridgeServer::getInstance().sendLog("INFO", "Attack started");
        return "Auto attack started";
    });
    
    bridge.registerCommand("stopautoattack", [](const std::string& params) -> std::string {
        BridgeServer::getInstance().sendLog("INFO", "Attack stopped");
        return "Auto attack stopped";
    });
    
    bridge.registerCommand("healinit", [](const std::string& params) -> std::string {
        BridgeServer::getInstance().sendLog("INFO", "Heal init");
        return "Heal initialized";
    });
    
    bridge.registerCommand("startautoheal", [](const std::string& params) -> std::string {
        BridgeServer::getInstance().sendLog("INFO", "Heal started, count: " + params);
        return "Auto heal started, count: " + params;
    });
    
    bridge.registerCommand("stopautoheal", [](const std::string& params) -> std::string {
        BridgeServer::getInstance().sendLog("INFO", "Heal stopped");
        return "Auto heal stopped";
    });
    
    bridge.registerCommand("starthunter", [](const std::string& params) -> std::string {
        BridgeServer::getInstance().sendLog("INFO", "Hunter started");
        return "Hunter started";
    });
    
    bridge.registerCommand("stophunter", [](const std::string& params) -> std::string {
        BridgeServer::getInstance().sendLog("INFO", "Hunter stopped");
        return "Hunter stopped";
    });
    
    bridge.registerCommand("dohunter", [](const std::string& params) -> std::string {
        return "Hunting...";
    });
    
    bridge.registerCommand("logs", [](const std::string& params) -> std::string {
        return "{\"logs\":[]}";
    });
    
    BridgeServer::getInstance().sendLog("INFO", "All commands registered");
}
