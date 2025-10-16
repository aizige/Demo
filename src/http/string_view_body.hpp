//
// Created by Aiziboy on 2025/10/13.
//

#ifndef UNTITLED1_STRING_BODY_VIEW_HPP
#define UNTITLED1_STRING_BODY_VIEW_HPP



#include <boost/beast/http/fields.hpp>
#include <boost/asio/buffer.hpp>
#include <string_view>

/**
 * @brief 一个 Boost.Beast Body 类型，它使用 std::string_view 来表示 HTTP 消息体。
 *
 * 这个 Body 类型是只读的 (read-only)，它不拥有数据的所有权。
 * 它主要用于高效地序列化（发送）一个已存在于内存中的字符串，而无需进行任何拷贝。
 *
 * @warning 因为不拥有数据，使用者必须保证 string_view 所指向的原始数据
 *          在整个异步写操作完成之前都保持有效。
 */
struct string_view_body
{
    // value_type 定义了 Body 存储的数据类型
    using value_type = std::string_view;

    /**
     * @brief 返回消息体的大小。
     * @param body 包含 string_view 的 value_type 实例。
     * @return 消息体的大小（以字节为单位）。
     */
    static std::uint64_t size(const value_type& body)
    {
        return body.size();
    }

    /**
     * @brief 获取一个用于序列化（写入）的缓冲区序列。
     * @param body 包含 string_view 的 value_type 实例。
     * @return 一个可转换为 ConstBufferSequence 的对象。
     */
    static auto data(const value_type& body)
    {
        return boost::asio::buffer(body.data(), body.size());
    }

    // --- Reader 和 Writer 的定义 ---
    // Reader 用于解析（读取）请求，因为我们只用它来发送，
    // 所以 Reader 部分可以是一个空的实现。

    class reader
    {
    public:
        template<bool isRequest, class Fields>
        explicit reader(boost::beast::http::header<isRequest, Fields>&, value_type&)
        {
            // 在构造时不做任何事情
        }

        void init(const boost::optional<std::uint64_t>&, boost::beast::error_code& ec)
        {
            ec = {};
        }

        template<class ConstBufferSequence>
        std::size_t put(const ConstBufferSequence&, boost::beast::error_code& ec)
        {
            ec = boost::beast::http::error::unexpected_body;
            return 0;
        }

        void finish(boost::beast::error_code& ec)
        {
            ec = {};
        }
    };

    // Writer 是可选的，但为了完整性，我们提供它。
    // 在我们的场景中，因为我们只使用 data() 函数，Writer 不会被直接调用。
    class writer
    {
    public:
        using const_buffers_type = boost::asio::const_buffer;

        template<bool isRequest, class Fields>
        explicit writer(boost::beast::http::header<isRequest, Fields> const&, value_type const& body)
            : body_(body)
        {
        }

        void init(boost::beast::error_code& ec)
        {
            ec = {};
        }

        boost::optional<std::pair<const_buffers_type, bool>> get(boost::beast::error_code& ec)
        {
            ec = {};
            return {{ boost::asio::buffer(body_), false }};
        }
    private:
        value_type const& body_;
    };
};


#endif //UNTITLED1_STRING_BODY_VIEW_HPP