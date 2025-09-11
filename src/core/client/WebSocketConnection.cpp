//
// Created by Aiziboy on 25-8-5.
//

#include "WebSocketConnection.hpp"

#include <spdlog/spdlog.h>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/beast/core/buffers_to_string.hpp>

#include "utils/finally.hpp"

namespace asio = boost::asio;


WebSocketConnection::WebSocketConnection(WsVariantStream stream, HttpRequest upgrade_request, std::shared_ptr<IWebSocketClientHandler> handler, std::string id)
    : stream_(std::move(stream)),
      initial_upgrade_request_(std::move(upgrade_request)),
      handler_(std::move(handler)),
      id_(std::move(id)),
      send_channel_(get_executor(), 1024){
    SPDLOG_DEBUG("WebSocketConnection [{}] 已创建.", id_);
}

WebSocketConnection::~WebSocketConnection() {
    SPDLOG_DEBUG("WebSocketConnection [{}] 已销毁.", id_);
}

boost::asio::any_io_executor WebSocketConnection::get_executor() const {
    return std::visit([](const auto& s) { return s->get_executor(); }, stream_);
}

bool WebSocketConnection::is_open() const {
    // 只有在握手成功且底层流打开时，连接才算打开
    return !is_closing_ && std::visit([](const auto& s) { return s->is_open(); }, stream_);
}

void WebSocketConnection::run() {
    // 启动两个并行的、独立的协程
    asio::co_spawn(get_executor(), [self = shared_from_this()] { return self->reader_loop(); }, asio::detached);
    asio::co_spawn(get_executor(), [self = shared_from_this()] { return self->writer_loop(); }, asio::detached);
}


boost::asio::awaitable<void> WebSocketConnection::send(WebSocketMessage message) {
    if (is_closing_) {
        throw boost::system::system_error(asio::error::connection_aborted, "WebSocket 连接正在关闭中");
    }
    auto [ec] = co_await send_channel_.async_send({}, std::move(message), asio::as_tuple(asio::use_awaitable));
    if (ec) {
        throw boost::system::system_error(ec, "发送消息到 WebSocket 发送队列失败");
    }
}



boost::asio::awaitable<void> WebSocketConnection::close(websocket::close_code code, std::string_view reason) {
    if (close_called_.exchange(true)) co_return;
    is_closing_ = true;

    // 关闭 channel 会唤醒并终止 reader/writer 循环
    send_channel_.close();

    try {
        co_await std::visit(
            [&](auto& s) -> asio::awaitable<void> {
                if (s->is_open()) {
                    boost::system::error_code ignored_ec;
                    co_await s->async_close(websocket::close_reason(code, reason), asio::redirect_error(asio::use_awaitable, ignored_ec));
                }
            },
            stream_
        );
    } catch (const std::exception& e) {
        SPDLOG_WARN("WebSocket [{}] 关闭时发生异常: {}", id_, e.what());
    }
}

boost::asio::awaitable<void> WebSocketConnection::reader_loop() {
    boost::system::error_code final_ec;
    websocket::close_reason close_reason;

    try {
        // --- 阶段一: WebSocket 握手 ---
        std::visit([this](auto& ws) {
            ws->auto_fragment(true); // 可选设置碎片化行为
            ws->read_message_max(10 * 1024 * 1024); // 设置最大读取消息体大小为10M（默认就是1M）
            // 启用压缩扩展
            websocket::permessage_deflate pmd;
            pmd.client_enable = true;

            ws->set_option(pmd);
            ws->set_option(websocket::stream_base::decorator(
                [](websocket::response_type& res) {
                    res.set(http::field::server, "MyWebSocketClient/1.0");
                }
            ));

            // 设置控制帧处理器（ping/pong/close）
            ws->control_callback([self = weak_from_this()](websocket::frame_type kind, boost::beast::string_view payload) {
                    if (auto locked_self = self.lock()) {
                        // Beast 默认会自动回复 PONG，这里只是为了记录日志
                        if (kind == websocket::frame_type::ping) {
                            SPDLOG_DEBUG("WebSocket [{}] Received PING, Beast will auto-reply PONG.", locked_self->id());
                        } else if (kind == websocket::frame_type::pong) {
                            SPDLOG_DEBUG("WebSocket [{}] Received PONG.", locked_self->id());
                        }
                    }
                }
            );
        }, stream_);

        // 握手操作
        co_await std::visit(
            [&](auto& s) { return s->async_handshake(initial_upgrade_request_.at(http::field::host), initial_upgrade_request_.target(), asio::use_awaitable); },
            stream_
        );

        SPDLOG_INFO("WebSocket [{}] 握手成功.", id_);
        asio::co_spawn(get_executor(), handler_->on_connect(shared_from_this()), asio::detached);

        // --- 阶段二: 持续读取循环 ---
        while (!is_closing_) {
            read_buffer_.clear();
            co_await std::visit(
                [&](auto& s) { return s->async_read(read_buffer_, asio::use_awaitable); },
                stream_
            );

            WebSocketMessage msg = boost::beast::buffers_to_string(read_buffer_.data());
            co_await handler_->on_message(std::move(msg));
        }
    } catch (const boost::system::system_error& e) {
        final_ec = e.code();
        // 正常的关闭错误不需要作为警告打印
        if (e.code() != asio::error::eof && e.code() != websocket::error::closed && e.code() != asio::error::operation_aborted) {
            SPDLOG_WARN("WebSocket [{}] reader_loop 因 system_error 退出: {}", id_, e.what());
        }
    } catch (const std::exception& e) {
        final_ec = asio::error::fault; // 一个通用的内部错误
        SPDLOG_ERROR("WebSocket [{}] reader_loop 因 exception 退出: {}", id_, e.what());
    }

    // --- 清理 ---
    if (is_open()) {
        close_reason = std::visit([](auto& s) { return s->reason(); }, stream_);
    }

    // 通知 handler 连接已断开
    handler_->on_disconnect(close_reason, final_ec);

    // 确保连接被彻底关闭
    co_await close();
}

boost::asio::awaitable<void> WebSocketConnection::writer_loop() {
    try {
        while (!is_closing_) {
            // 从发送 channel 中等待新的消息
            auto [ec, msg_to_send] = co_await send_channel_.async_receive(asio::as_tuple(asio::use_awaitable));

            if (ec) {
                // Channel 被关闭，意味着连接正在关闭，正常退出
                break;
            }

            // 将消息写入网络
            co_await std::visit(
                [&](auto& s) { return s->async_write(asio::buffer(msg_to_send), asio::use_awaitable); },
                stream_
            );
        }
    } catch (...) {
        // 写循环中的任何异常都被认为是致命的。我们不在这里记录日志，
        // 因为 reader_loop 中的异常会提供更根本的错误信息。
        // 我们只需确保连接被关闭。
    }
    // 写循环退出时，也触发一下 close，以确保所有资源被清理
    co_await close();
}
