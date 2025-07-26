/*
 * nghttp2 - HTTP/2 C Library
 *
 * Copyright (c) 2014 Tatsuhiro Tsujikawa
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
// We wrote this code based on the original code which has the
// following license:
//
// io_service_pool.hpp
// ~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2013 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_IO_SERVICE_POOL_H
#define ASIO_IO_SERVICE_POOL_H

#include "nghttp2_config.h"

#include <vector>
#include <memory>
#include <future>

#include <boost/noncopyable.hpp>
#include <thread> // **改用 std::thread**

#include <boost/noncopyable.hpp>
#include <boost/asio/io_context.hpp> // **使用 io_context**
#include <boost/asio/executor_work_guard.hpp> // **使用 executor_work_guard**
#include <boost/asio/any_io_executor.hpp> // Asio 的通用 executor

#include "asio_http2.h"

namespace nghttp2 {

namespace asio_http2 {

/// A pool of io_service objects.
class io_service_pool : private boost::noncopyable {
public:

  /// Construct the io_context pool.
  explicit io_service_pool(std::size_t pool_size);

  /// Destructor.
  ~io_service_pool();

  /// Run all io_context objects in the pool. Each io_context runs in its own thread.
  void run();

  /// Stop all io_context objects in the pool gracefully.
  void stop();

  /// Wait for all threads in the pool to exit.
  void join();



  /// Get an io_context to use in a round-robin fashion.
  boost::asio::io_context& get_io_service();

private:
  /// The pool of io_contexts.
  std::vector<std::shared_ptr<boost::asio::io_context>> io_contexts_;

  /// The work guards that keep the io_contexts running.
  // **类型已更改**
  std::vector<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_guards_;

  /// The threads that will run the io_contexts.
  // **使用 std::vector<std::thread> 管理线程**
  std::vector<std::thread> threads_;

  /// The next io_context to use for a connection.
  std::size_t next_io_context_;
};

} // namespace asio_http2

} // namespace nghttp2

#endif // ASIO_IO_SERVICE_POOL_H
