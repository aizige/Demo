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
#include "asio_client_session_tcp_impl.h"

namespace nghttp2 {
namespace asio_http2 {
namespace client {

    session_tcp_impl::session_tcp_impl(
        boost::asio::io_context& io_context,
        session_handler* handler,
        const std::chrono::seconds& connect_timeout)
        : session_impl(io_context, handler, connect_timeout),
          socket_(io_context)
    {}

    session_tcp_impl::session_tcp_impl(
        boost::asio::io_context& io_context,
        session_handler* handler,
        const boost::asio::ip::tcp::endpoint& local_endpoint,
        const std::chrono::seconds& connect_timeout)
        : session_impl(io_context, handler, connect_timeout),
          socket_(io_context) {
  socket_.open(local_endpoint.protocol());
  boost::asio::socket_base::reuse_address option(true);
  socket_.set_option(option);
  socket_.bind(local_endpoint);
}

session_tcp_impl::~session_tcp_impl() {}

void session_tcp_impl::start_connect(const resolver_results_type &endpoints) {
  auto self = std::static_pointer_cast<session_tcp_impl>(shared_from_this());

  boost::asio::async_connect(
      socket_, endpoints,
      [self](const boost::system::error_code& ec, const tcp::endpoint& /*ep*/) {
          if (self->stopped()) return;

          if (ec) {
              // 通过 handler 报告连接错误
              if (self->handler_) {
                  self->handler_->on_error(ec);
              }
              self->stop(); // 发生错误，停止会话
              return;
          }

          // TCP 连接成功，现在设置 nghttp2 session
          if (!self->setup_session()) {
              // setup_session 内部如果失败，会自己调用 on_error
              self->stop();
              return;
          }


          // 设置 TCP_NODELAY，对低延迟有益
        self->socket().set_option(tcp::no_delay(true));

        // 启动读写循环和 PING 机制
        self->do_write();
        self->do_read();
        self->start_ping();

        // **最后，通过 handler 通知上层连接完全就绪**
        if (self->handler_) {
            self->handler_->on_connect();
        }
      }
  );
}

tcp::socket &session_tcp_impl::socket() { return socket_; }

void session_tcp_impl::read_socket(
std::function<void(const boost::system::error_code& ec, std::size_t n)> h) {
        socket_.async_read_some(boost::asio::buffer(rb_), std::move(h));
}

void session_tcp_impl::write_socket(
std::function<void(const boost::system::error_code& ec, std::size_t n)> h) {
        boost::asio::async_write(socket_, boost::asio::buffer(wb_, wblen_), std::move(h));
}

void session_tcp_impl::shutdown_socket() {
        boost::system::error_code ignored_ec;
        socket_.close(ignored_ec);
}

} // namespace client
} // namespace asio_http2
} // namespace nghttp2
