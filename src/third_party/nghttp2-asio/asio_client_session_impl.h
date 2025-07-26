#ifndef ASIO_CLIENT_SESSION_IMPL_H
#define ASIO_CLIENT_SESSION_IMPL_H

#include "nghttp2_config.h"

#include <boost/array.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp> // 使用 steady_timer
#include <boost/system/error_code.hpp>

#include "asio_http2_client.h"
#include "template.h"

namespace nghttp2 {
namespace asio_http2 {
namespace client {

class stream;

using boost::asio::ip::tcp;
using resolver_results_type = tcp::resolver::results_type;



class session_impl : public std::enable_shared_from_this<session_impl> {
public:
  // **构造函数现在需要一个 handler 指针**
  session_impl(boost::asio::io_context& io_context,
               session_handler* handler,
               const std::chrono::seconds& connect_timeout);
               
  virtual ~session_impl();

  void start_resolve(const std::string &host, const std::string &port);

  // 这些函数现在是内部实现细节，可以设为 private
  // void connected(...);
  // void not_connected(...);

  // **移除**: 回调注册函数被 handler 接口替代
  // void on_connect();
  // void on_error(error_cb cb);

  int write_trailer(stream &strm, header_map h);
  void cancel(stream &strm, uint32_t error_code);
  void resume(stream &strm);

  std::unique_ptr<stream> create_stream();
  std::unique_ptr<stream> pop_stream(int32_t stream_id);
  stream *create_push_stream(int32_t stream_id);
  stream *find_stream(int32_t stream_id);

  const request *submit(boost::system::error_code &ec,
                        const std::string &method, const std::string &uri,
                        generator_cb cb, header_map h, priority_spec spec);

  // 这些是纯虚函数，需要由子类（tcp_impl, tls_impl）实现
  virtual void start_connect(const resolver_results_type& endpoints) = 0;
  virtual tcp::socket& socket() = 0;
  virtual void read_socket(std::function<void(const boost::system::error_code &ec, std::size_t n)> h) = 0;
  virtual void write_socket(std::function<void(const boost::system::error_code &ec, std::size_t n)> h) = 0;
  virtual void shutdown_socket() = 0;

  void shutdown();
  boost::asio::io_context& io_context();
  void signal_write();
  void read_timeout(const std::chrono::seconds& t);
  void stop();
  bool stopped() const;
  
  // 这两个函数用于回调保护，保持不变
  void enter_callback();
  void leave_callback();

protected:
    // 子类需要这些方法来驱动会话
    bool setup_session();
    void do_read();
    void do_write();
    void start_ping();

    session_handler* handler_; // **持有 handler 指针**
    boost::asio::io_context& io_context_;

    boost::array<uint8_t, 8192> rb_;
    boost::array<uint8_t, 65536> wb_;
    std::size_t wblen_;

private:

  
  bool should_stop() const;

  void call_error_cb(const boost::system::error_code &ec);
  void handle_deadline();

  void handle_ping(const boost::system::error_code &ec);


  tcp::resolver resolver_;


  std::map<int32_t, std::unique_ptr<stream>> streams_;

  // **使用 steady_timer 和 chrono**
  boost::asio::steady_timer deadline_;
  std::chrono::seconds connect_timeout_;
  std::chrono::seconds read_timeout_;
  boost::asio::steady_timer ping_;

  nghttp2_session *session_;
  


  const uint8_t *data_pending_;
  std::size_t data_pendinglen_;

  bool writing_;
  bool inside_callback_;
  bool stopped_;
};

} // namespace client
} // namespace asio_http2
} // namespace nghttp2

#endif // ASIO_CLIENT_SESSION_IMPL_H