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
// io_service_pool.cpp
// ~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2013 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "asio_io_service_pool.h"

namespace nghttp2 {

namespace asio_http2 {
  io_service_pool::io_service_pool(std::size_t pool_size) : next_io_context_(0) {
    if (pool_size == 0) {
      throw std::runtime_error("io_context_pool size is 0");
    }

    // Pre-construct all the io_contexts and their work guards.
    for (std::size_t i = 0; i < pool_size; ++i) {
      // **使用 io_context**
      auto ioc = std::make_shared<boost::asio::io_context>();
      io_contexts_.push_back(ioc);

      // **使用 make_work_guard 来创建 work guard**
      // emplace_back 可以直接在 vector 尾部构造对象
      work_guards_.emplace_back(boost::asio::make_work_guard(ioc->get_executor()));
    }
  }

  io_service_pool::~io_service_pool() {
    // 确保在析构时，所有线程都被优雅地停止
    stop();
    join();
  }

  void io_service_pool::run() {
    // Create a pool of threads to run all of the io_contexts.
    threads_.reserve(io_contexts_.size());
    for (auto& ioc : io_contexts_) {
      // **使用 std::thread 来启动线程**
      threads_.emplace_back([&ioc]() {
          ioc->run();
      });
    }
  }

  void io_service_pool::join() {
    // Wait for all threads in the pool to exit.
    for (auto& t : threads_) {
      if (t.joinable()) {
        t.join();
      }
    }
  }

  void io_service_pool::stop() {
    // Explicitly reset all work guards. This allows io_context::run() to exit once
    // all other work is done.
    work_guards_.clear();

    // 如果希望立即停止，而不是等待当前任务完成，可以调用 ioc->stop()
    // for (auto& ioc : io_contexts_) {
    //   ioc->stop();
    // }
  }

  boost::asio::io_context& io_service_pool::get_io_service() {
    // Use a round-robin scheme to choose the next io_context to use.
    auto& ioc = *io_contexts_[next_io_context_];
    ++next_io_context_;
    if (next_io_context_ == io_contexts_.size()) {
      next_io_context_ = 0;
    }
    return ioc;
  }

  // 这个函数在新的设计中不再需要，因为我们不再暴露整个列表
  // const std::vector<std::shared_ptr<boost::asio::io_context>>&
  // io_service_pool::io_services() const {
  //   return io_contexts_;
  // }

} // namespace asio_http2

} // namespace nghttp2
