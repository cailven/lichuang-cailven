#include "app_upload_server.h"

#include "script_game.h"
#include "script_storage.h"

#include <esp_log.h>

#include <cstdlib>
#include <cstring>
#include <string>

#define TAG "AppUploadServer"

static constexpr size_t kMaxBody = 128 * 1024;

httpd_handle_t AppUploadServer::server_ = nullptr;

// 简单的单页应用: 列表 + 上传 + 删除 + API 速查
static const char* kPage =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>小智游戏安装器</title><style>"
    "body{font-family:sans-serif;background:#10141a;color:#e0e0e0;max-width:640px;margin:16px auto;padding:0 12px}"
    "h2{color:#7ecb8f} .card{background:#1a2028;border-radius:12px;padding:12px 16px;margin:14px 0}"
    "button{background:#2e7d32;color:#fff;border:0;border-radius:8px;padding:6px 14px;margin-left:8px}"
    "button.del{background:#8a3a3a} input{background:#0d1117;color:#e0e0e0;border:1px solid #333;border-radius:6px;padding:5px}"
    "li{margin:6px 0;display:flex;align-items:center} ul{list-style:none;padding:0}"
    "pre{background:#0d1117;padding:10px;border-radius:8px;overflow-x:auto;font-size:13px}"
    "</style></head><body><h2>\xF0\x9F\x8E\xAE 小智游戏安装器</h2>"
    "<div class=card><h3>已安装</h3><ul id=list>加载中...</ul></div>"
    "<div class=card><h3>安装新游戏</h3>"
    "<p>名字 <input id=name placeholder=\"mygame\"></p>"
    "<p><input type=file id=file accept=\".lua\"></p>"
    "<p><button onclick=up()>安装</button><span id=msg></span></p></div>"
    "<div class=card><h3>脚本 API</h3><pre>"
    "app = { title=\"标题\", color=0xRRGGBB }\n"
    "canvas.fill(c)  canvas.rect(x,y,w,h,c)  canvas.text(x,y,s,c)\n"
    "game.set_tick_period(ms)  game.exit()  game.time()\n"
    "game.random(n)  game.millis()  print(s)\n"
    "game.time() 返回 h,m,s,年,月,日 (联网校时)\n"
    "on_start()  on_tick()  on_touch(evt,x,y)  -- evt: down/move/up\n"
    "标准库: string table math | 画布 320x200 | 颜色 0xRRGGBB\n"
    "中文需等字体推送后显示, 回桌面图标即时刷新</pre></div>"
    "<script>"
    "async function refresh(){let r=await fetch('/api/apps');let j=await r.json();"
    "let ul=document.getElementById('list');ul.innerHTML='';"
    "if(!j.apps.length){ul.innerHTML='<li>(空)</li>';return;}"
    "for(const a of j.apps){let li=document.createElement('li');"
    "li.textContent=a;let b=document.createElement('button');b.className='del';"
    "b.textContent='删除';b.onclick=async()=>{await fetch('/app/'+encodeURIComponent(a),{method:'DELETE'});refresh()};"
    "li.appendChild(b);ul.appendChild(li);}}"
    "async function up(){let f=document.getElementById('file').files[0];"
    "if(!f){document.getElementById('msg').textContent=' 先选 .lua 文件';return;}"
    "let n=document.getElementById('name').value.trim()||f.name.replace(/\\.lua$/,'');"
    "if(!/^[-_A-Za-z0-9]+$/.test(n)){document.getElementById('msg').textContent=' 名字只能字母数字-_';return;}"
    "let r=await fetch('/upload?name='+encodeURIComponent(n),{method:'PUT',body:f});"
    "document.getElementById('msg').textContent=r.ok?(' '+(await r.text())):' 失败';refresh();}"
    "refresh();</script></body></html>";

esp_err_t AppUploadServer::PageHandler(httpd_req_t* req) {
    httpd_resp_set_type(req, HTTPD_TYPE_TEXT);
    httpd_resp_set_hdr(req, "Content-Type", "text/html; charset=utf-8");
    httpd_resp_send(req, kPage, strlen(kPage));
    return ESP_OK;
}

esp_err_t AppUploadServer::ListHandler(httpd_req_t* req) {
    std::string body = "{\"apps\":[";
    bool first = true;
    for (auto& name : ScriptStorage::ListApps()) {
        if (!first) body += ",";
        body += "\"" + name + "\"";
        first = false;
    }
    body += "]}";
    httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    httpd_resp_send(req, body.c_str(), body.size());
    return ESP_OK;
}

esp_err_t AppUploadServer::UploadHandler(httpd_req_t* req) {
    char query[128] = {0};
    char name[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "name", name, sizeof(name));
    } else {
        name[0] = '\0';
    }
    std::string app_name = name;
    if (!ScriptStorage::IsValidName(app_name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
        return ESP_FAIL;
    }

    size_t total = req->content_len;
    if (total == 0 || total > kMaxBody) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty or too large");
        return ESP_FAIL;
    }
    std::string content;
    content.resize(total);
    size_t received = 0;
    while (received < total) {
        int n = httpd_req_recv(req, &content[received], total - received);
        if (n <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_FAIL;
        }
        received += (size_t)n;
    }
    if (!ScriptStorage::WriteApp(app_name, content)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }
    // 装完立即用沙箱跑一遍脚本顶层, 语法/顶层错误当场反馈 (运行期错误仍需打开游戏才知道)
    ScriptAppMeta meta;
    if (!ScriptGameApp::PeekMeta(content, meta)) {
        std::string msg = "installed, but script error: top-level run failed";
        httpd_resp_send(req, msg.c_str(), msg.size());
        return ESP_OK;
    }
    std::string msg = "installed: " + meta.title;
    httpd_resp_send(req, msg.c_str(), msg.size());
    return ESP_OK;
}

esp_err_t AppUploadServer::DeleteHandler(httpd_req_t* req) {
    // 路径形如 /app/<name>, 直接解析 URI
    std::string name;
    const char* uri = req->uri;
    const char* prefix = "/app/";
    if (strncmp(uri, prefix, strlen(prefix)) == 0) {
        name = uri + strlen(prefix);
        size_t q = name.find('?');
        if (q != std::string::npos) {
            name = name.substr(0, q);
        }
    }
    ScriptStorage::DeleteApp(name);
    const char* msg = "deleted";
    httpd_resp_send(req, msg, strlen(msg));
    return ESP_OK;
}

bool AppUploadServer::Start() {
    if (server_ != nullptr) {
        return true;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8080;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;  // 支持 /app/* 通配删除路径
    esp_err_t err = httpd_start(&server_, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start upload server: %s", esp_err_to_name(err));
        return false;
    }
    httpd_uri_t page = {.uri = "/", .method = HTTP_GET, .handler = PageHandler, .user_ctx = nullptr};
    httpd_uri_t list = {.uri = "/api/apps", .method = HTTP_GET, .handler = ListHandler, .user_ctx = nullptr};
    httpd_uri_t upload = {.uri = "/upload", .method = HTTP_PUT, .handler = UploadHandler, .user_ctx = nullptr};
    httpd_uri_t del = {.uri = "/app/*", .method = HTTP_DELETE, .handler = DeleteHandler, .user_ctx = nullptr};
    httpd_register_uri_handler(server_, &page);
    httpd_register_uri_handler(server_, &list);
    httpd_register_uri_handler(server_, &upload);
    httpd_register_uri_handler(server_, &del);
    ESP_LOGI(TAG, "Game installer ready on port 8080");
    return true;
}
