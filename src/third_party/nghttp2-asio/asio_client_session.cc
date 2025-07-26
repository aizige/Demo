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
#include "nghttp2_config.h"

#include "asio_http2_client.h"

#include "asio_client_session_tcp_impl.h"
#include "asio_client_session_tls_impl.h"
#include "asio_common.h"
#include "template.h"

namespace nghttp2 {
namespace asio_http2 {
namespace client {

using boost::asio::ip::tcp;

  // 明文 TCP 构造函数
  session::session(boost::asio::io_context& io_context,
                   const std::string& host,
                   const std::string& service,
                   const std::chrono::seconds& connect_timeout)
      : impl_(std::make_shared<session_tcp_impl>(io_context, this, connect_timeout))
  {
    impl_->start_resolve(host, service);
  }

  // TLS 构造函数
  session::session(boost::asio::io_context& io_context,
                   boost::asio::ssl::context& tls_ctx,
                   const std::string& host,
                   const std::string& service,
                   const std::chrono::seconds& connect_timeout)
      : impl_(std::make_shared<session_tls_impl>(io_context, this, tls_ctx, host, connect_timeout))
  {
    impl_->start_resolve(host, service);
  }


  session::~session() {}

  session::session(session &&other) noexcept = default;
  session &session::operator=(session &&other) noexcept = default;


  // --- 新的 handler 接口实现 ---

  void session::on_connect() {
    // 这是底层 impl 通知我们连接已成功。
    // 在这个封装层，我们可能不需要做任何事，或者可以打印一条日志。
    // 如果有外部的回调需求，可以在这里触发。
    // spdlog::debug("nghttp2-asio session connected.");
  }

  void session::on_error(const boost::system::error_code& ec) {
    // 底层 impl 通知我们发生了错误。
    // spdlog::error("nghttp2-asio session error: {}", ec.message());
  }



  void session::shutdown() const {
    if (impl_) impl_->shutdown();
  }

  boost::asio::io_context& session::io_service() const {
    return impl_->io_context();
  }

  const request *session::submit(boost::system::error_code &ec,
                               const std::string &method,
                               const std::string &uri, header_map h,
                               priority_spec prio) const {
    return impl_->submit(ec, method, uri, {}, std::move(h), std::move(prio));
  }

  const request *session::submit(boost::system::error_code &ec,
                                 const std::string &method,
                                 const std::string &uri, std::string data,
                                 header_map h, priority_spec prio) const {
    return impl_->submit(ec, method, uri, string_generator(std::move(data)),
                         std::move(h), std::move(prio));
  }

  const request *session::submit(boost::system::error_code &ec,
                                 const std::string &method,
                                 const std::string &uri, generator_cb cb,
                                 header_map h, priority_spec prio) const {
    return impl_->submit(ec, method, uri, std::move(cb), std::move(h),
                         std::move(prio));
  }


  void session::read_timeout(const std::chrono::seconds& t) {
    if (impl_) impl_->read_timeout(t);
  }

priority_spec::priority_spec(const int32_t stream_id, const int32_t weight,
                             const bool exclusive)
    : valid_(true) {
  nghttp2_priority_spec_init(&spec_, stream_id, weight, exclusive);
}

const nghttp2_priority_spec *priority_spec::get() const {
  if (!valid_) {
    return nullptr;
  }

  return &spec_;
}

bool priority_spec::valid() const { return valid_; }

} // namespace client
} // namespace asio_http2
} // namespace nghttp2
