//
// Created by ubuntu on 2025/7/21.
//

#ifndef UNTITLED1_CONNECTION_HELPERS_HPP
#define UNTITLED1_CONNECTION_HELPERS_HPP
#include <boost/asio/ip/tcp.hpp>

inline bool is_connection_healthy(boost::asio::ip::tcp::socket& socket) {
    if (!socket.is_open()) {
        return false;
    }

    // --- 核心的健康检查逻辑 ---
    boost::system::error_code ec;

    // 1. 将 socket 设置为非阻塞模式
    socket.non_blocking(true, ec);
    if (ec) return false; // 如果设置失败，认为连接有问题

    // 2. 尝试读取 0 字节
    char dummy_buf[1];
    socket.read_some(boost::asio::buffer(dummy_buf, 0), ec);

    // 3. 立即将 socket 恢复为阻塞模式
    //    (因为我们框架的其他部分都期望是阻塞/异步模式)
    socket.non_blocking(false, ec);
    // 我们忽略恢复时的错误，因为主要关心的是 read 的结果

    // 4. 分析 read_some 的结果
    if (ec == boost::asio::error::would_block) {
        // **这是我们期望的“健康”状态**
        // would_block 意味着 "现在没数据可读，但连接是好的"
        return true;
    }

    // 如果 ec 是 eof, connection_reset, broken_pipe 或任何其他错误，
    // 都意味着这个连接已经死亡或处于不确定状态。
    return false;
}
#endif //UNTITLED1_CONNECTION_HELPERS_HPP