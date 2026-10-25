#pragma once

// 局域网游戏安装器: 板上 HTTP 服务 (端口 8080)。
// 手机/电脑连同一 WiFi, 浏览器打开 http://<板子IP>:8080 即可:
// 查看/删除已装游戏, 上传 .lua 安装新游戏 —— 全程不需要 USB 烧录。

#include <esp_http_server.h>

class AppUploadServer {
public:
    static bool Start();
    static bool IsRunning() { return server_ != nullptr; }

private:
    static httpd_handle_t server_;
    static esp_err_t PageHandler(httpd_req_t* req);      // GET /
    static esp_err_t ListHandler(httpd_req_t* req);      // GET /api/apps
    static esp_err_t UploadHandler(httpd_req_t* req);    // PUT /upload?name=xxx
    static esp_err_t DeleteHandler(httpd_req_t* req);    // DELETE /app/xxx
};
