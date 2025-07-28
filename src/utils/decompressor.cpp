//
// Created by ubuntu on 2025/7/7.
//

#include <array>

#include "decompressor.hpp"

#include <spdlog/spdlog.h>
#include <format>
#include <fmt/core.h> // <-- 包含 fmt 库的核心头文件
// 定义一个合理的缓冲区大小，用于每次读取 zlib 的输出
constexpr size_t CHUNK_SIZE = 16384; // 16 KB

void Decompressor::initialize(Format format) {
    if (m_initialized) {
        cleanup();
    }
    m_stream = {};
    m_stream.zalloc = Z_NULL;
    m_stream.zfree = Z_NULL;
    m_stream.opaque = Z_NULL;
    m_format = format;

    int windowBits;
    if (format == Format::GZIP) {
        windowBits = 15 + 16; // gzip 格式
    } else {
        // DEFLATE
        // zlib 的默认行为 (15) 通常能同时处理 zlib-wrap 和 raw deflate。
        // 为了最大的兼容性，我们可以让 zlib 自动检测。
        // 15 + 32 会让 zlib 自动检测 gzip 或 zlib 格式。
        // 但 -15 是最标准的 raw deflate。我们先用默认的。
        windowBits = 15;
    }

    int ret = inflateInit2(&m_stream, windowBits);
    if (ret != Z_OK) {
        throw std::runtime_error("Decompressor: inflateInit2 failed.");
    }
    m_initialized = true;
}

void Decompressor::cleanup() {
    if (m_initialized) {
        (void)inflateEnd(&m_stream);
        m_initialized = false;
    }
}

Decompressor::Decompressor(Format format) {
    m_buffer.resize(CHUNK_SIZE);
    initialize(format);
}

Decompressor::~Decompressor() {
    cleanup();
}

// 移动构造函数
Decompressor::Decompressor(Decompressor&& other) noexcept
    : m_stream(other.m_stream), m_buffer(std::move(other.m_buffer)),
      m_initialized(other.m_initialized), m_format(other.m_format) {
    // 将源对象置于一个有效的、可析构的状态
    other.m_initialized = false;
}

// 移动赋值运算符
Decompressor& Decompressor::operator=(Decompressor&& other) noexcept {
    if (this != &other) {
        cleanup();
        m_stream = other.m_stream;
        m_buffer = std::move(other.m_buffer);
        m_initialized = other.m_initialized;
        m_format = other.m_format;
        other.m_initialized = false;
    }
    return *this;
}

void Decompressor::reset(Format new_format) {
    if (!m_initialized || m_format != new_format) {
        // 如果格式改变了，我们需要完全重新初始化
        initialize(new_format);
    } else {
        // 否则，只重置流状态
        int ret = inflateReset(&m_stream);
        if (ret != Z_OK) {
            throw std::runtime_error("Decompressor: inflateReset failed.");
        }
    }
}



std::string Decompressor::decompress(std::string_view compressed_data) {
    reset(m_format); // 确保每次解压都是从一个干净的状态开始

    std::string decompressed_output;

    m_stream.avail_in = compressed_data.size();
    m_stream.next_in = (Bytef*)compressed_data.data();

    int ret;
    do {
        m_stream.avail_out = m_buffer.size();
        m_stream.next_out = (Bytef*)m_buffer.data();

        ret = inflate(&m_stream, Z_NO_FLUSH);

        switch (ret) {
        case Z_STREAM_ERROR:
            throw std::runtime_error("GzipDecompressor: inflate failed with Z_STREAM_ERROR.");
        case Z_NEED_DICT:
        case Z_DATA_ERROR:
        case Z_MEM_ERROR:
            (void)inflateEnd(&m_stream); // Clean up
            m_initialized = false;
            throw std::runtime_error("GzipDecompressor: inflate failed with critical error.");
        default:
                ;
        }

        // 计算这次解压了多少数据
        size_t have = m_buffer.size() - m_stream.avail_out;
        // 将解压出的数据追加到输出字符串
        decompressed_output.append(m_buffer.data(), have);
    }
    while (m_stream.avail_out == 0);

    // inflate 应该在处理完所有数据后返回 Z_STREAM_END
    if (ret != Z_STREAM_END) {
        // 如果数据流没有正常结束，可能意味着压缩数据不完整或已损坏
        SPDLOG_WARN("GzipDecompressor: Gzip stream did not end cleanly. It might be truncated.");
    }
    if (ret == Z_STREAM_ERROR) {
        std::stringstream ss;
        ss << "Decompressor: inflate failed with Z_STREAM_ERROR. Format was " << (m_format == Format::GZIP ? "GZIP" : "DEFLATE");
        throw std::runtime_error(ss.str());
    }

    return decompressed_output;
}
