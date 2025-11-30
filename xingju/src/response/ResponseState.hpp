//
// Created by Aiziboy on 2025/11/23.
//

#ifndef XINGJU_RESPONSE_STATE_HPP
#define XINGJU_RESPONSE_STATE_HPP

#include <string>

// 1. 定义状态结构 (对应 Java 枚举的成员变量)
struct StateInfo {
    int code;
    std::string message;
};

// 2. 定义常量集合 (对应 Java 的枚举项)
namespace ResponseState {
    // --- 成功状态 ---
    static const StateInfo SUCCESS             = {0, "OK"};
    static const StateInfo LOGIN_SUCCESS       = {20001, "登录成功"};

    // --- 失败状态 ---
    // 通用失败
    static const StateInfo FAILED              = {40000, "操作失败"};
    static const StateInfo VALIDATION_FAILED   = {40001, "参数校验失败"};
    static const StateInfo VERIFIER_CODE_ERROR = {40001, "验证码错误"};

    // 认证授权相关
    static const StateInfo ACCOUNT_NOT_LOGIN        = {40100, "请先登录"};
    static const StateInfo ACCOUNT_LOGIN_EXPIRED    = {40101, "登录已过期，请重新登录"};
    static const StateInfo ACCOUNT_LOGIN_FAILED     = {40102, "登录失败，账号或密码错误"};
    static const StateInfo ACCOUNT_BAN              = {40104, "账号已被封禁"};
    static const StateInfo ACCOUNT_ALREADY_EXISTING = {40103, "账户已存在"};
    static const StateInfo ERROR_403                = {40003, "无访问权限"};

    // 资源相关
    static const StateInfo RESOURCE_NOT_FOUND  = {40400, "请求的资源不存在"};
    static const StateInfo MEDIA_TYPE_ERROR    = {60600, "不支持的文件类型"};
    static const StateInfo MEDIA_NULL          = {60000, "上传的文件不能为null"};
    static const StateInfo CONTENT_TYPE_ERROR  = {60001, "不支持的 Content Type"};

    // 系统/服务异常
    static const StateInfo SERVICE_ERROR       = {50000, "服务端内部错误"};
    static const StateInfo REQUEST_TIMEOUT     = {50004, "请求超时，请重试"};
    static const StateInfo ERROR_404           = {40004, "内容不存在，迷路了"};

   static const StateInfo  ERROR_INSTANCE_NOT_FOUND     = {41000, "实例未注册"};
   static const StateInfo  ERROR_INSTANCE_EXISTS        = {41100, "实例已存在"};
   static const StateInfo  ERROR_VERSION_OUTDATED       = {41200, "版本号过旧，需要全量同步"};
   static const StateInfo  ERROR_INVALID_TOKEN          = {41300, "Token 无效或缺失"};
   static const StateInfo  ERROR_MISSING_FIELDS         = {41400, "缺少必要字段"};
   static const StateInfo  ERROR_UNAUTHORIZED_ACCESS    = {41500, "未授权访问"};

}

#endif //XINGJU_RESPONSE_STATE_HPP