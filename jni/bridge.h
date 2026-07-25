#ifndef BRIDGE_H
#define BRIDGE_H

#include <string>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>

using CommandCallback = std::function<std::string(const std::string&)>;

class BridgeServer {
public:
    static BridgeServer& getInstance();
    
    void start(int port = 27042);
    void stop();
    bool isRunning() const { return running_; }
    
    void registerCommand(const std::string& cmd, CommandCallback callback);
    void sendLog(const std::string& level, const std::string& msg);
    void broadcast(const std::string& data);
    
    // 设置外部 Lua 执行函数
    void setLuaExecutor(std::function<bool(const std::string&)> executor);
    bool execLua(const std::string& code);
    
private:
    BridgeServer() = default;
    ~BridgeServer() { stop(); }
    BridgeServer(const BridgeServer&) = delete;
    BridgeServer& operator=(const BridgeServer&) = delete;
    
    void serverLoop();
    void handleClient(int client_fd);
    std::string processCommand(const std::string& raw_json);
    
    static std::string escapeJSON(const std::string& s);
    static std::string getJSONString(const std::string& json, const std::string& key);
    
    int server_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread server_thread_;
    std::mutex mutex_;
    std::map<std::string, CommandCallback> commands_;
    std::vector<int> client_fds_;
    
    std::function<bool(const std::string&)> lua_executor_;
};

// ============================================
// 便捷宏，用于注册命令
// ============================================
#define REGISTER_COMMAND(name, body) \
    BridgeServer::getInstance().registerCommand(name, [](const std::string& params) -> std::string body)

#endif