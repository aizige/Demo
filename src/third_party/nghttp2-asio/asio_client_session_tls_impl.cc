/*
 * nghttp2 - HTTP/2 C Library
 *
 * Copyright (c) 2015 Tatsuhiro Tsujikawa
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "asio_client_session_tls_impl.h"
#include "asio_common.h"
#include <boost/asio/connect.hpp> // **需要包含这个头文件**
#include <boost/asio/ssl/host_name_verification.hpp> // **新的头文件**
namespace nghttp2 {
    namespace asio_http2 {
        namespace client {
            session_tls_impl::session_tls_impl(
                boost::asio::io_context& io_context,
                session_handler* handler,
                boost::asio::ssl::context& tls_ctx,
                const std::string& host,
                const std::chrono::seconds& connect_timeout)
                // 将 handler 传递给基类**
                : session_impl(io_context, handler, connect_timeout),
                  socket_(io_context, tls_ctx)
            {

            }

            session_tls_impl::~session_tls_impl() {
            }

            void session_tls_impl::start_connect(const resolver_results_type& endpoints) {
                auto self = std::static_pointer_cast<session_tls_impl>(shared_from_this());

                // 1. 先连接底层的 TCP socket
                boost::asio::async_connect(
                    socket_.next_layer(), // **操作 next_layer()**
                    endpoints,
                    [self](const boost::system::error_code& ec, const tcp::endpoint&) {
                        if (self->stopped()) return;
                        if (ec) {
                            if (self->handler_) self->handler_->on_error(ec);
                            self->stop();
                            return;
                        }

                        // 2. TCP 连接成功，再进行 TLS 握手
                        self->socket_.async_handshake(
                            boost::asio::ssl::stream_base::client,
                            [self](const boost::system::error_code& handshake_ec) {
                                if (self->stopped()) return;
                                if (handshake_ec) {
                                    if (self->handler_) self->handler_->on_error(handshake_ec);
                                    self->stop();
                                    return;
                                }

                                // 3. TLS 握手成功！执行后续通用逻辑
                                if (!self->setup_session()) {
                                    self->stop();
                                    return;
                                }

                                self->socket().set_option(tcp::no_delay(true));
                                self->do_write();
                                self->do_read();
                                self->start_ping();

                                if (self->handler_) {
                                    self->handler_->on_connect();
                                }
                            }
                        );
                    }
                );
            }

            // 返回最底层的 socket
            tcp::socket& session_tls_impl::socket() { return socket_.next_layer(); }


            // 在 ssl::stream 上读写
            void session_tls_impl::read_socket(
                std::function<void(const boost::system::error_code& ec, std::size_t n)> h) {
                socket_.async_read_some(boost::asio::buffer(rb_), std::move(h));
            }

            void session_tls_impl::write_socket(
             std::function<void(const boost::system::error_code& ec, std::size_t n)> h) {
                boost::asio::async_write(socket_, boost::asio::buffer(wb_, wblen_), std::move(h));
            }

            // 优雅地关闭
            void session_tls_impl::shutdown_socket() {
                boost::system::error_code ec;
                // 尝试优雅关闭 SSL，忽略错误
                socket_.shutdown(ec);
                // 关闭底层 socket
                socket_.lowest_layer().close(ec);
            }

        } // namespace client
    } // namespace asio_http2
} // namespace nghttp2
