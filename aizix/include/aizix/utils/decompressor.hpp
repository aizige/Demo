//
// Created by Aiziboy on 2025/7/7.
//

#ifndef AIZIX_GZIPDECOMPRESSOR_HPP
#define AIZIX_GZIPDECOMPRESSOR_HPP


#include <string>
#include <string_view>
#include <vector>


// 引入 zlib 头文件
#include <zlib.h>

class Decompressor {
public:
    // 定义支持的压缩格式
    enum class Format {
        GZIP,
        DEFLATE
    };

    Decompressor(Format format);
    ~Decompressor();

    // 禁止拷贝和移动，因为 z_stream 的状态是唯一的
    Decompressor(const Decompressor&) = delete;
    Decompressor& operator=(const Decompressor&) = delete;

    // 允许移动
    Decompressor(Decompressor&& other) noexcept;
    Decompressor& operator=(Decompressor&& other) noexcept;

    // 主解压函数
    std::string decompress(std::string_view compressed_data);

    void reset(Format new_format);

private:


    void initialize(Format format);
    void cleanup();


    z_stream m_stream;
    bool m_initialized = false;
    std::vector<char> m_buffer; // 可复用的内部缓冲区
    Format m_format;
};


#endif //AIZIX_GZIPDECOMPRESSOR_HPP