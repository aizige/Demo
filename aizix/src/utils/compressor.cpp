// Compressor.cpp

#include <aizix/utils/compressor.hpp>

#include <stdexcept>

// 为 zlib 的输出块定义一个合理的缓冲区大小。
constexpr size_t CHUNK_SIZE = 16384; // 16 KB

void Compressor::initialize(Format format, int level) {
    // 如果已经初始化过，先清理旧资源
    if (m_initialized) {
        cleanup();
    }
    m_stream = {}; // 将流结构体清零
    m_stream.zalloc = Z_NULL; // 使用 zlib 默认的内存分配函数
    m_stream.zfree = Z_NULL;
    m_stream.opaque = Z_NULL;

    m_format = format;
    m_level = level;

    // `windowBits` 参数决定了压缩方法和头部格式。
    // 15 是 zlib 窗口大小的默认值和最大值。
    // +16 是一个特殊的标志，告诉 zlib 在输出中添加 GZIP 格式的头部和尾部。
    // 对于 DEFLATE 格式，我们只使用默认的窗口大小。
    int windowBits = (format == Format::GZIP) ? (15 + 16) : 15;

    // `memLevel` 控制 zlib 内部压缩状态所使用的内存量。
    // 8 是 zlib 的默认值，在内存使用和压缩速度之间提供了良好的平衡。
    int memLevel = 8;

    // `deflateInit2` 是初始化 zlib 进行压缩的现代化函数。
    // 它允许我们精确指定格式、级别、策略等多种参数。
    int ret = deflateInit2(&m_stream, m_level, Z_DEFLATED, windowBits, memLevel, Z_DEFAULT_STRATEGY);

    if (ret != Z_OK) {
        throw std::runtime_error("Compressor: deflateInit2 failed.");
    }
    m_initialized = true;
}

void Compressor::cleanup() {
    if (m_initialized) {
        // `deflateEnd` 必须被调用，以释放所有内部状态占用的内存。
        (void)deflateEnd(&m_stream);
        m_initialized = false;
    }
}

Compressor::Compressor(Format format, int level) {
    m_buffer.resize(CHUNK_SIZE);
    initialize(format, level);
}

Compressor::~Compressor() {
    cleanup();
}

// 移动构造函数
Compressor::Compressor(Compressor&& other) noexcept
    : m_stream(other.m_stream), m_buffer(std::move(other.m_buffer)),
      m_initialized(other.m_initialized), m_format(other.m_format), m_level(other.m_level) {
    // 将源对象置于一个有效的、可被安全析构的状态
    other.m_initialized = false;
}

// 移动赋值运算符
Compressor& Compressor::operator=(Compressor&& other) noexcept {
    if (this != &other) {
        cleanup(); // 先释放当前对象的资源
        m_stream = other.m_stream;
        m_buffer = std::move(other.m_buffer);
        m_initialized = other.m_initialized;
        m_format = other.m_format;
        m_level = other.m_level;
        // 将源对象置于安全状态
        other.m_initialized = false;
    }
    return *this;
}

void Compressor::reset(Format new_format, int new_level) {
    // 如果格式或级别发生了变化，需要进行一次完全的重新初始化。
    if (!m_initialized || m_format != new_format || m_level != new_level) {
        initialize(new_format, new_level);
    } else {
        // 否则，只重置流的状态，这是一个非常轻量的操作。
        (void)deflateReset(&m_stream);
    }
}



std::string Compressor::compress(std::string_view data) {
    // 1. 如果输入为空，zlib 压缩后也不是空字符串（会有头部和尾部）。
    //    为了简化，并且在 HTTP 场景中通常无需压缩空 body，我们直接返回空字符串。
    //    如果需要严格的 zlib 空输入压缩，这里的逻辑会更复杂一些。
    if (data.empty()) {
        return "";
    }

    // 2. 重置流状态，确保每次压缩都是全新的开始。
    reset(m_format, m_level);

    std::string compressed_output;
    // 预估一个合理的容量，减少后续内存分配的次数。
    compressed_output.reserve(data.size() / 2);

    // 3. 设置输入
    m_stream.avail_in = static_cast<uInt>(data.size());
    m_stream.next_in = (Bytef*)data.data();

    int ret = Z_OK;

    // --- 压缩分为两个阶段 ---

    // 4. 阶段一：处理所有输入数据
    //    只要还有输入数据 (m_stream.avail_in > 0)，就持续调用 deflate。
    //    这里使用 Z_NO_FLUSH，让 zlib 自行决定何时输出数据块。
    do {
        m_stream.avail_out = m_buffer.size();
        m_stream.next_out = (Bytef*)m_buffer.data();

        ret = deflate(&m_stream, Z_NO_FLUSH);
        if (ret == Z_STREAM_ERROR) {
            throw std::runtime_error("Compressor: deflate failed with Z_STREAM_ERROR.");
        }

        size_t have = m_buffer.size() - m_stream.avail_out;
        if (have > 0) {
            compressed_output.append(m_buffer.data(), have);
        }
    } while (m_stream.avail_out == 0); // 如果输出缓冲区被填满了，说明可能还有数据，继续循环

    // 此时，所有输入数据都已经被 deflate 消耗 (m_stream.avail_in == 0)。
    // 但可能还有一些压缩后的数据在 zlib 的内部缓冲区中。

    // 5. 阶段二：终结压缩流 (Flushing)
    //    我们必须持续调用 deflate 并使用 Z_FINISH，直到它返回 Z_STREAM_END。
    //    这会确保所有内部缓冲被清空，并且 GZIP/DEFLATE 的尾部数据被写入。
    do {
        m_stream.avail_out = m_buffer.size();
        m_stream.next_out = (Bytef*)m_buffer.data();

        ret = deflate(&m_stream, Z_FINISH);
        if (ret == Z_STREAM_ERROR) {
            throw std::runtime_error("Compressor: deflate(Z_FINISH) failed with Z_STREAM_ERROR.");
        }

        size_t have = m_buffer.size() - m_stream.avail_out;
        if (have > 0) {
            compressed_output.append(m_buffer.data(), have);
        }
    } while (ret != Z_STREAM_END); // 这是唯一的、正确的结束条件

    // 6. 流程结束，返回结果
    return compressed_output;
}
/*std::string Compressor::compress(std::string_view data) {
    // 对空数据进行特殊处理，直接返回空字符串
    if (data.empty()) {
        return "";
    }

    // 重置流状态，确保我们从一个干净的状态开始处理新的数据。
    reset(m_format, m_level);

    std::string compressed_output;
    // 根据经验，预分配输入大小的一半左右作为初始容量，以减少后续的内存重分配。
    compressed_output.reserve(data.size() / 2);

    // 设置输入数据
    m_stream.avail_in = data.size();
    m_stream.next_in = (Bytef*)data.data();

    int flush_mode;
    int ret;

    // 循环处理输入数据，直到所有数据都被消耗完
    do {
        // 设置输出缓冲区
        m_stream.avail_out = m_buffer.size();
        m_stream.next_out = (Bytef*)m_buffer.data();

        // 决定刷新模式。只有在所有输入数据都提供给 zlib 后，
        // 在最后一次调用 `deflate` 时，我们才使用 Z_FINISH。
        // Z_FINISH 会告诉 zlib 写完所有挂起的输出，并附上最终的尾部数据。
        flush_mode = (m_stream.avail_in == 0) ? Z_FINISH : Z_NO_FLUSH;

        ret = deflate(&m_stream, flush_mode);

        if (ret == Z_STREAM_ERROR) {
            // 这是一个严重的、不可恢复的流状态错误。
            throw std::runtime_error("Compressor: deflate failed with Z_STREAM_ERROR.");
        }

        // 计算本次 `deflate` 调用在我们的缓冲区里产生了多少压缩数据。
        size_t have = m_buffer.size() - m_stream.avail_out;

        // 如果有新数据产生，就把它追加到我们的输出字符串中。
        if (have > 0) {
            compressed_output.append(m_buffer.data(), have);
        }

    // 只要 `deflate` 填满了整个输出缓冲区（意味着可能还有更多数据待处理），
    // 或者我们正在结束流程（直到 ret 变为 Z_STREAM_END），循环就继续。
    // Z_BUF_ERROR 在使用 Z_FINISH 时是可能发生的，表示输出缓冲区不足，需要继续循环。
    } while (m_stream.avail_out == 0 || ret == Z_BUF_ERROR);

    // 循环结束后，如果使用了 Z_FINISH，`deflate` 的返回值必须是 Z_STREAM_END。
    // 如果不是，说明压缩流程没有正常完成。
    if (ret != Z_STREAM_END) {
        throw std::runtime_error("Compressor: compression failed to complete cleanly.");
    }

    return compressed_output;
}*/
