//
// Created by Aiziboy on 2025/7/7.
//

#ifndef AIZIX_COMPRESSOR_HPP
#define AIZIX_COMPRESSOR_HPP


#include <string>
#include <string_view>
#include <vector>
#include <stdexcept>

// 引入 zlib 头文件
#include <zlib.h>

/**
 * @class Compressor
 * @brief 一个生产级的、使用 zlib 的数据压缩器。
 *
 * 此类提供了使用 GZIP 或 DEFLATE 格式来压缩数据的功能。
 * 它的设计目标是可复用和高效率，能够良好地管理其内部的
 * z_stream 状态和缓冲区。
 */
class Compressor {
public:
    // 定义支持的压缩格式
    enum class Format {
        GZIP, // GZIP 格式 (通常用于文件和 HTTP)
        DEFLATE // DEFLATE 格式 (zlib 原始格式)
    };

    /**
     * @brief Compressor 构造函数。
     * @param format 要使用的压缩格式 (GZIP 或 DEFLATE)，默认为 GZIP。
     * @param level 压缩级别 (范围 0-9, 或 Z_DEFAULT_COMPRESSION)。
     *              - Z_DEFAULT_COMPRESSION (-1): 默认级别，速度和压缩率的良好平衡。
     *              - 0: 不进行压缩。
     *              - 1: 最快速度。
     *              - 9: 最佳压缩率。
     */
    explicit Compressor(Format format = Format::GZIP, int level = Z_DEFAULT_COMPRESSION);

    /**
     * @brief 析构函数。
     *        负责安全地释放 zlib 占用的资源。
     */
    ~Compressor();

    // z_stream 的状态是唯一的，因此禁止拷贝操作。
    Compressor(const Compressor &) = delete;

    Compressor &operator=(const Compressor &) = delete;

    // 允许移动操作，以支持在不同作用域间转移所有权。
    Compressor(Compressor &&other) noexcept;

    Compressor &operator=(Compressor &&other) noexcept;


    /**
     * @brief 压缩一块数据。
     *
     * @param data 要压缩的输入数据。
     * @return 一个包含压缩后数据的 std::string。
     */
    std::string compress(std::string_view data);

    /**
     * @brief 重置压缩器的内部状态，为一次新的压缩任务做准备。
     *
     * 如果新的格式或级别与当前不同，将会完全重新初始化内部流。
     * 否则，只会执行一个轻量级的状态重置。
     *
     * @param new_format 要使用的新压缩格式。
     * @param new_level 要使用的新压缩级别。
     */
    void reset(Format new_format, int new_level = Z_DEFAULT_COMPRESSION);

private:


    /**
     * @brief 初始化 z_stream，为压缩做准备。
     * @param format 压缩格式。
     * @param level 压缩级别。
     */
    void initialize(Format format, int level);

    /**
     * @brief 清理并释放 z_stream 占用的资源。
     */
    void cleanup();

    z_stream m_stream; // zlib 的核心流对象，包含了所有压缩状态。
    bool m_initialized = false; // 标记流是否已成功初始化。
    std::vector<char> m_buffer; // 用于存放 zlib 输出数据块的可复用内部缓冲区。
    Format m_format; // 当前配置的压缩格式。
    int m_level; // 当前配置的压缩级别。
};


#endif //AIZIX_COMPRESSOR_HPP
