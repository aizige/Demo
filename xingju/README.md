# 🌌 星聚 · 服务注册与发现系统

> 星聚：服务如星，聚而不乱。 “乡月淹今夕，文星聚此堂。” —— 欧阳修  

---

## ✨ 项目简介

**星聚** 基于 C++23 协程与 Boost Asio 构建的，一个轻量级、可扩展的服务注册与发现系统，专为高性能分布式系统设计。  
它支持客户端主动注册与健康上报，服务端提供注册管理、健康检查、服务发现等功能，适用于微服务架构、边缘计算、云原生部署等场景。

---

## 🚀 特性亮点

- 🌌 **异步高性能**：基于 C++23 协程 + Boost Asio 实现。
- 🛰️ **主动心跳机制**：客户端定期 ping，服务端自动剔除失活节点。
- 🧭 **多维服务定位**：支持国家 / 城市 / IDC 多层级部署标识。
- 🔐 **安全认证机制**：支持 token 验证、接口权限控制。
- 🛠️ **可视化管理面板**：支持 Web UI 管理服务状态（建议通过网关暴露）。

---

## 📦 项目结构
```ini
starcluster/ 
├── server/ # 服务端核心逻辑 
├── client/ # 客户端 SDK 
├── web/ # 管理面板（可选） 
├── docs/ # 文档与接口说明 
├── config/ # 配置模板 
├── conanfile.txt # Conan 依赖管理 
└── README.md
```
---

## 🔧 依赖管理（Conan）

### conanfile.txt 示例
```ini
[requires]
boost/1.83.0
openssl/3.1.3
nlohmann_json/3.11.2
spdlog/1.13.0

[generators]
CMakeDeps
CMakeToolchain

[options]
boost:shared=False
openssl:shared=False
spdlog:header_only=True
```
---

## 构建步骤
```ini
pip install conan
conan profile detect
conan install . --output-folder=build --build=missing
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=conan_toolchain.cmake
cmake --build .`
```
---

## ⚙️ 配置文件示例（server.yaml）
```yaml
listen_port: 9000
auth_token: "your-secure-token"
ping_check_interval: 10
ping_timeout: 30
enable_tls: false
web_panel:
  enabled: true
  listen_port: 8080
  auth:
    username: admin
    password: secret

```
---

## 📡 接口文档

### 注册服务实例
`POST /register`

请求 Body : application/json

| 字段名                      | 类型     | 必填    | 说明              |
|--------------------------|--------|-------|-----------------|
| instance_id              | string | true  | 实例ID (全局唯一)     |
| service_name             | string | true  | 服务名称            |
| ip                       | string | false | 此服务实例ip,会从连接自动获 |
| public_ip                | string | false | 公网ip            |
| port                     | uint16 | true  | 服务端口            |
| tls                      | bool   | false | 服务是否开启TLS       |
| check_interval           | uint64 | true  | 健康上报间隔          |
| check_critical_timeout   | uint64 | true  | 不健康服务实例删除时间     |
| idc                      | string | true  | IDC             |

请求示例
```json
{
  "status": "UP",
  "instance_id": "inst-004",
  "service_name": "user-service",
  "ip": "192.168.1.1",
  "public_ip": "203.0.113.4",
  "port": 8080,
  "tls": false,
  "check_interval": 10,
  "check_critical_timeout": 30,
  "idc": "jp-idc-1"
}

```
成功响应
```json
{
  "code": 0,
  "message": "注册实例成功",
  "data": null
}

```
失败响应
```json
{
    "code": 40000,
    "message": "缺少必填字段: `instance_id` or `service_name` or `port(0 - 65535)` or `check_interval(0 - UINT64_MAX)` or `check_critical_timeout(0 - UINT64_MAX)`",
    "data": null
}
```
---

### ping
`PUT /ping?instanceId=<id>`

客户端心跳上报，同时支持增量同步。

请求 Body
```json
{
  "last_seen_version": 42
}

```
成功响应（有更新）
```json
{
  "code": "SUCCESS",
  "message": "Heartbeat received",
  "current_version": 45,
  "updates": [
    {
      "event": "service_registered",
      "instance_id": "abc123",
      "service_name": "order-service",
      "ip": "192.168.1.10",
      "port": 8080,
      "tls": false,
      "location": {
        "country": "JP",
        "city": "Osaka",
        "idc": "IDC-1"
      }
    },
    {
      "event": "service_removed",
      "instance_id": "xyz789",
      "service_name": "payment-service"
    }
  ]
}

```

成功响应（无更新）

```json
{
  "code": "SUCCESS",
  "message": "Heartbeat received",
  "current_version": 45,
  "updates": []
}

```

错误响应（实例未注册）
```json
{
  "code": "ERR_INSTANCE_NOT_FOUND",
  "message": "Instance not found. Please re-register.",
  "register_url": "/register"
}

```
错误响应（版本号过旧）
```json
{
  "code": "ERR_VERSION_OUTDATED",
  "message": "Version too old, full sync required",
  "current_version": 100,
  "full_services": [ ... 全量服务列表 ... ]
}

```

---

## 查询服务列表
`GET /get?service_name=<name>`

响应示例
```json
{
  "code": "SUCCESS",
  "services": [
    {
      "instance_id": "abc123",
      "ip": "192.168.1.10",
      "port": 8080,
      "tls": false,
      "location": {
        "country": "JP",
        "city": "Osaka",
        "idc": "IDC-1"
      },
      "last_ping": "2025-11-16T06:00:00Z"
    }
  ]
}

```
---

## 🔄 版本号管理机制

- 全局版本号：星聚维护一个全局递增版本号，每次服务注册/注销/更新时 +1。

- 网关版本号：网关在心跳请求中携带 last_seen_version。

- 服务端比较：

    - 若 last_seen_version < current_version → 返回增量更新事件。

    - 若 last_seen_version == current_version → 返回空更新。

- 一致性保证：

  - 网关在成功接收更新后，将本地版本号更新为 current_version。

  - 若版本号落后过多，服务端返回全量列表。

---

## 🛠️ 错误恢复机制
- 版本号过旧：返回 ERR_VERSION_OUTDATED，并附带全量服务列表。

- 网络异常：下次心跳时携带旧版本号，服务端返回增量或全量更新。

- 实例未注册：返回 ERR_INSTANCE_NOT_FOUND，网关需重新调用 /register。

---

## 📜 状态码与错误码

|  错误码 | 含义  |
|---|---|
| SUCCESS  |  心跳/请求成功 |
| ERR_INSTANCE_NOT_FOUND  | 实例未注册  |
| ERR_INSTANCE_EXISTS  | 实例已存在  |
|  ERR_VERSION_OUTDATED |  网关版本号过旧，需要全量同步 |
|  ERR_INVALID_TOKEN | Token 无效或缺失  |
| ERR_MISSING_FIELDS  |  缺少必要字段 |
|  ERR_UNAUTHORIZED_ACCESS |  未授权访问 |
|  ERR_INTERNAL_ERROR | 服务端内部错误  |

---

## ⏱️ 推荐心跳间隔
- 默认：10 秒

- 最大：30 秒

- 说明：心跳间隔越短，网关感知更新越及时；间隔过长可能导致服务发现延迟。
	
	
	
	
	
	
	
