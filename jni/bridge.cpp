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
        // 尝试数字值
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
    
    // 设置超时
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
        
        // 新连接
        if (FD_ISSET(server_fd_, &readfds)) {
            int client = accept(server_fd_, nullptr, nullptr);
            if (client >= 0) {
                LOGI("New client: %d", client);
                std::lock_guard<std::mutex> lock(mutex_);
                client_fds_.push_back(client);
                // 发送欢迎消息
                std::string welcome = "{\"type\":\"log\",\"level\":\"INFO\",\"msg\":\"Bridge Server ready\"}\n";
                write(client, welcome.c_str(), welcome.length());
            }
        }
        
        // 客户端数据
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
                
                // 处理 HTTP 请求（浏览器访问）
                if (strncmp(buf, "GET ", 4) == 0 || strncmp(buf, "POST ", 5) == 0) {
                    handleClient(fd);
                    close(fd);
                    it = client_fds_.erase(it);
                    continue;
                }
                
                // 处理 TCP JSON 命令（可能包含多条，用 \n 分隔）
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
// HTTP 处理（内嵌 Web 控制台）
// ============================================
void BridgeServer::handleClient(int client_fd) {
    char buf[65536];
    int n = read(client_fd, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = '\0';
    
    std::string request(buf);
    
    // 提取请求路径
    std::string method, path;
    std::istringstream iss(request);
    iss >> method >> path;
    
    // 路由
    if (path == "/" || path == "/index.html") {
        // 返回内嵌控制台 HTML
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
.log-line{padding:1px 0}
.log-time{color:#555}
.log-INFO{color:#00ff88}
.log-WARN{color:#ffaa00}
.log-ERROR{color:#ff4455}
.flex{display:flex;align-items:center;gap:8px}
.spacer{flex:1}
.badge{display:inline-block;width:8px;height:8px;border-radius:50%;background:#555}
.badge.on{background:#00ff88;box-shadow:0 0 6px #00ff88}
</style>
</head>
<body>

<div class="header">
  <h1>🎮 游戏辅助控制台</h1>
  <span>Zygisk Injection · Bridge :27042</span>
</div>

<div class="card">
  <div class="card-title">⚔️ 冰原巨兽 <span class="status" id="st-battle">未加载</span></div>
  <div class="btns">
    <button onclick="loadMod('auto_battle',this)" id="btn-battle-load">📂 加载</button>
    <button onclick="cmd('initbattle')">🔧 初始化</button>
    <button class="on" onclick="cmd('startbattle')">▶ 启动</button>
    <button class="danger" onclick="cmd('stopbattle')">■ 停止</button>
  </div>
</div>

<div class="card">
  <div class="card-title">🐻 巨熊行动 <span class="status" id="st-bear">未加载</span></div>
  <div class="btns">
    <button onclick="loadMod('auto_bear',this)" id="btn-bear-load">📂 加载</button>
    <button class="warn" onclick="cmd('createmarch')">🚩 开集结</button>
    <button class="on" onclick="cmd('startauto')">▶ 启动</button>
    <button class="danger" onclick="cmd('stopauto')">■ 停止</button>
  </div>
</div>

<div class="card">
  <div class="card-title">🏰 自动王城 <span class="status" id="st-attack">未加载</span></div>
  <div class="btns">
    <button onclick="loadMod('auto_attack',this)" id="btn-attack-load">📂 加载</button>
    <button onclick="cmd('attackinit')">🔧 初始化</button>
    <button class="on" onclick="cmd('startautoattack')">▶ 启动</button>
    <button class="danger" onclick="cmd('stopautoattack')">■ 停止</button>
  </div>
</div>

<div class="card">
  <div class="card-title">💊 自动治疗 <span class="status" id="st-heal">未加载</span></div>
  <div class="btns flex">
    <button onclick="loadMod('auto_heal',this)" id="btn-heal-load">📂 加载</button>
    <button onclick="cmd('healinit')">🔧 初始化</button>
    <input type="number" id="heal-count" value="50" min="1" max="9999" style="width:55px">
    <button class="on" onclick="cmd('startautoheal',document.getElementById('heal-count').value)">▶ 启动</button>
    <button class="danger" onclick="cmd('stopautoheal')">■ 停止</button>
  </div>
</div>

<div class="card">
  <div class="card-title">👹 召唤Boss <span class="status" id="st-hunter">未加载</span></div>
  <div class="btns">
    <button onclick="loadMod('auto_hunter',this)" id="btn-hunter-load">📂 加载</button>
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

function log(msg,level='INFO'){
  var el=document.getElementById('log');
  var t=new Date().toLocaleTimeString();
  el.innerHTML+='<div class="log-line"><span class="log-time">['+t+']</span> <span class="log-'+level+'">'+msg+'</span></div>';
  el.scrollTop=el.scrollHeight;
}

async function cmd(name,params){
  try{
    var r=await fetch('/api/'+name,{
      method:'POST',
      headers:{'Content-Type':'application/json'},
      body:JSON.stringify({params:params||''})
    });
    var j=await r.json();
    log('✅ '+name+': '+(j.data||'OK'),j.status=='error'?'ERROR':'INFO');
  }catch(e){
    log('❌ '+name+': '+e.message,'ERROR');
  }
}

async function loadMod(name,btn){
  if(loaded[name]){
    await cmd('unload_module',name);
    loaded[name]=false;
    btn.textContent='📂 加载';
    btn.classList.remove('danger');
    document.getElementById('st-'+name.split('_')[1]).textContent='未加载';
    document.getElementById('st-'+name.split('_')[1]).className='status';
    return;
  }
  btn.textContent='⏳...';
  btn.disabled=true;
  await cmd('load_module',name);
  loaded[name]=true;
  btn.textContent='🗑️ 卸载';
  btn.classList.add('danger');
  btn.disabled=false;
  var mod=name.split('_')[1]||name;
  var st=document.getElementById('st-'+mod);
  if(st){st.textContent='已加载';st.className='status on';}
}

// 定时拉取日志
setInterval(async function(){
  try{
    var r=await fetch('/api/logs');
    var j=await r.json();
    if(j.logs) j.logs.forEach(function(l){log(l.msg,l.level);});
  }catch(e){}
},3000);

log('🚀 控制台就绪');
log('📡 监听 127.0.0.1:27042');
</script>
</body>
</html>
)HTML";
        write(client_fd, html, strlen(html));
    }
    else if (path.find("/api/") == 0) {
        // API 路由
        std::string api_name = path.substr(5); // 去掉 /api/
        
        // 提取 body
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
        // 404
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
    
    LOGI("Command: %s, params: %s", cmd.c_str(), params.c_str());
    
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

// ============================================
// 注册命令
// ============================================
void BridgeServer::registerCommand(const std::string& cmd, CommandCallback cb) {
    commands_[cmd] = cb;
    LOGI("Registered: %s", cmd.c_str());
}

// ============================================
// 广播日志
// ============================================
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

// ============================================
// Lua 执行器
// ============================================
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
// 全局辅助函数
// ============================================
void start_bridge_server(int port) {
    BridgeServer::getInstance().start(port);
}

// 存储已加载的模块列表
static std::vector<std::string> loaded_modules;
static std::mutex modules_mutex;

void register_all_commands() {
    auto& bridge = BridgeServer::getInstance();
    
    // ===== 基础命令 =====
    REGISTER_COMMAND("ping", {
        return "pong";
    });
    
    REGISTER_COMMAND("get_status", {
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
    
    // ===== 模块管理 =====
    REGISTER_COMMAND("load_module", {
        std::lock_guard<std::mutex> lock(modules_mutex);
        if (std::find(loaded_modules.begin(), loaded_modules.end(), params) == loaded_modules.end()) {
            loaded_modules.push_back(params);
        }
        bridge.sendLog("INFO", "Module loaded: " + params);
        
        // 执行模块初始化 Lua 代码
        // bridge.execLua("require('" + params + "')");
        
        return "Module " + params + " loaded";
    });
    
    REGISTER_COMMAND("unload_module", {
        std::lock_guard<std::mutex> lock(modules_mutex);
        auto it = std::find(loaded_modules.begin(), loaded_modules.end(), params);
        if (it != loaded_modules.end()) {
            loaded_modules.erase(it);
        }
        bridge.sendLog("INFO", "Module unloaded: " + params);
        return "Module " + params + " unloaded";
    });
    
    // ===== 执行 Lua =====
    REGISTER_COMMAND("exec_lua", {
        bridge.sendLog("INFO", "Exec Lua: " + params.substr(0, 80));
        bool ok = bridge.execLua(params);
        return ok ? "Lua executed" : "Lua execution failed";
    });
    
    // ===== 冰原巨兽 =====
    REGISTER_COMMAND("initbattle", {
        bridge.sendLog("INFO", "初始化冰原巨兽");
        return "Battle initialized";
    });
    
    REGISTER_COMMAND("startbattle", {
        bridge.sendLog("INFO", "▶ 启动冰原巨兽");
        return "Battle started";
    });
    
    REGISTER_COMMAND("stopbattle", {
        bridge.sendLog("INFO", "■ 停止冰原巨兽");
        return "Battle stopped";
    });
    
    REGISTER_COMMAND("searchboss", {
        bridge.sendLog("INFO", "搜索Boss, 等级: " + params);
        return "Searching boss level " + params;
    });
    
    // ===== 巨熊行动 =====
    REGISTER_COMMAND("startauto", {
        bridge.sendLog("INFO", "▶ 启动巨熊行动");
        return "Auto bear started";
    });
    
    REGISTER_COMMAND("stopauto", {
        bridge.sendLog("INFO", "■ 停止巨熊行动");
        return "Auto bear stopped";
    });
    
    REGISTER_COMMAND("createmarch", {
        bridge.sendLog("INFO", "🚩 创建集结");
        return "March created";
    });
    
    REGISTER_COMMAND("addtargetuid", {
        bridge.sendLog("INFO", "添加目标UID: " + params);
        return "Target added: " + params;
    });
    
    REGISTER_COMMAND("removetargetuid", {
        bridge.sendLog("INFO", "移除目标UID: " + params);
        return "Target removed: " + params;
    });
    
    REGISTER_COMMAND("gettargetuids", {
        return "[]";
    });
    
    // ===== 自动王城 =====
    REGISTER_COMMAND("attackinit", {
        bridge.sendLog("INFO", "初始化王城攻击");
        return "Attack initialized";
    });
    
    REGISTER_COMMAND("startautoattack", {
        bridge.sendLog("INFO", "▶ 启动自动王城");
        return "Auto attack started";
    });
    
    REGISTER_COMMAND("stopautoattack", {
        bridge.sendLog("INFO", "■ 停止自动王城");
        return "Auto attack stopped";
    });
    
    // ===== 自动治疗 =====
    REGISTER_COMMAND("healinit", {
        bridge.sendLog("INFO", "初始化治疗");
        return "Heal initialized";
    });
    
    REGISTER_COMMAND("startautoheal", {
        bridge.sendLog("INFO", "▶ 启动自动治疗, 数量: " + params);
        return "Auto heal started, count: " + params;
    });
    
    REGISTER_COMMAND("stopautoheal", {
        bridge.sendLog("INFO", "■ 停止自动治疗");
        return "Auto heal stopped";
    });
    
    // ===== 召唤Boss =====
    REGISTER_COMMAND("starthunter", {
        bridge.sendLog("INFO", "▶ 启动召唤Boss");
        return "Hunter started";
    });
    
    REGISTER_COMMAND("stophunter", {
        bridge.sendLog("INFO", "■ 停止召唤Boss");
        return "Hunter stopped";
    });
    
    REGISTER_COMMAND("dohunter", {
        return "Hunting...";
    });
    
    // ===== 日志拉取（用于 Web 控制台轮询）=====
    REGISTER_COMMAND("logs", {
        return "{\"logs\":[]}";
    });
    
    bridge.sendLog("INFO", "All commands registered");
}