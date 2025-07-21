//
// Created by Aiziboy on 2025/7/20.
//

#include "decompression_manager.hpp"


namespace utils::compression {

    // 使用 thread_local 关键字
    // 这会为每个线程创建一个独立的 decompressor_gzip 实例
    Decompressor& DecompressionManager::get_thread_local_gzip_decompressor() {
        thread_local Decompressor decompressor_gzip(Decompressor::Format::GZIP);
        return decompressor_gzip;
    }

    Decompressor& DecompressionManager::get_thread_local_deflate_decompressor() {
        thread_local Decompressor decompressor_deflate(Decompressor::Format::DEFLATE);
        return decompressor_deflate;
    }

    std::string DecompressionManager::gzip_decompress(std::string_view compressed_data) {
        // 获取当前线程的 gzip 解压器并使用它
        return get_thread_local_gzip_decompressor().GzipDecompress(compressed_data);
    }

    std::string DecompressionManager::deflate_decompress(std::string_view compressed_data) {
        // 获取当前线程的 deflate 解压器并使用它
        return get_thread_local_deflate_decompressor().DeflateDecompress(compressed_data);
    }

} // namespace utils::compression