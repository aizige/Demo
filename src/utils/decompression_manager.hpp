//
// Created by Aiziboy on 2025/7/20.
//

#ifndef UNTITLED1_DECOMPRESSION_MANAGER_HPP
#define UNTITLED1_DECOMPRESSION_MANAGER_HPP


#include "decompressor.hpp" // 包含你有状态的解压器类
#include <string>
#include <string_view>
#include <memory>

namespace utils::compression {

    /**
     * @brief 提供一个线程安全的、高性能的解压接口。
     *
     * 内部使用 thread_local 来为每个工作线程维护一个独立的 Decompressor 实例，
     * 从而避免了锁竞争和状态冲突。
     */
    class DecompressionManager {
    public:
        /**
         * @brief 线程安全地解压 gzip 数据。
         * @param compressed_data 压缩数据。
         * @return 解压后的字符串。
         */
        static std::string gzip_decompress(std::string_view compressed_data);

        /**
         * @brief 线程安全地解压 deflate 数据。
         * @param compressed_data 压缩数据。
         * @return 解压后的字符串。
         */
        static std::string deflate_decompress(std::string_view compressed_data);

    private:
        // 私有辅助函数，用于获取当前线程的解压器实例
        static Decompressor& get_thread_local_gzip_decompressor();
        static Decompressor& get_thread_local_deflate_decompressor();
    };

}


#endif //UNTITLED1_DECOMPRESSION_MANAGER_HPP