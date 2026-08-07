#include <cstdint>
#include <algorithm>
#include <limits>
#include <uchardet.h>
#include <zlib.h>
#include <optional>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

#include "TextStream.h"

#include <opencv2/core/hal/interface.h>
#include <spdlog/spdlog.h>

#include "MsgIntf.h"
#include "UtilStreams.h"
#include "tjsError.h"
#include "CharacterSet.h"
#include "BinaryStream.h"

static std::string G_DefaultReadEncoding = "UTF-8";
static std::mutex G_DefaultReadEncodingMutex;

struct ModeNumberResult {
    bool present = false;
    bool valid = true;
    bool has_digits = false;
    std::uint64_t value = 0;
};

static ModeNumberResult parseModeNumberStrict(const tjs_char *mode,
                                              tjs_char key) noexcept {
    ModeNumberResult result;
    const tjs_char *p = TJS_strchr(mode, key);
    if(p == nullptr)
        return result;

    result.present = true;
    ++p;
    if(*p < TJS_W('0') || *p > TJS_W('9')) {
        // c/z have historically accepted a bare mode character.  Let the
        // caller apply that mode's default while still rejecting bare o.
        return result;
    }
    result.has_digits = true;
    do {
        const std::uint64_t digit = static_cast<std::uint64_t>(*p - TJS_W('0'));
        if(result.value >
           (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            result.valid = false;
            return result;
        }
        result.value = result.value * 10 + digit;
        ++p;
    } while(*p >= TJS_W('0') && *p <= TJS_W('9'));

    // A repeated mode key is ambiguous and is almost certainly a malformed
    // mode string.
    if(TJS_strchr(p, key) != nullptr)
        result.valid = false;
    return result;
}

static bool hasNonAsciiBytes(const unsigned char *raw, size_t size) {
    for(size_t i = 0; i < size; i++) {
        if(raw[i] >= 0x80)
            return true;
    }
    return false;
}

static bool isValidUTF8(const unsigned char *raw, size_t size) {
    size_t i = 0;
    while(i < size) {
        if(raw[i] < 0x80) {
            i++;
            continue;
        }
        int cont = 0;
        if((raw[i] & 0xE0) == 0xC0)
            cont = 1;
        else if((raw[i] & 0xF0) == 0xE0)
            cont = 2;
        else if((raw[i] & 0xF8) == 0xF0)
            cont = 3;
        else
            return false;
        i++;
        while(cont-- > 0) {
            if(i >= size || (raw[i] & 0xC0) != 0x80)
                return false;
            i++;
        }
    }
    return true;
}

std::string checkTextEncoding(const void *buf, size_t size,
                              std::uint8_t &bomSize) {
    auto raw = static_cast<const unsigned char *>(buf);
    bomSize = 0;
    if(!raw || size == 0)
        return {};
    std::string encoding;
    // --- 检查 BOM ---
    if(size >= 4 && raw[0] == 0xFF && raw[1] == 0xFE && raw[2] == 0x00 &&
       raw[3] == 0x00) {
        // UTF-32LE BOM (must precede UTF-16LE)
        bomSize = 4;
        encoding = "UTF-32LE";
    } else if(size >= 4 && raw[0] == 0x00 && raw[1] == 0x00 &&
              raw[2] == 0xFE && raw[3] == 0xFF) {
        // UTF-32BE BOM
        bomSize = 4;
        encoding = "UTF-32BE";
    } else if(size >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        // UTF-16LE BOM
        bomSize = 2;
        encoding = "UTF-16LE";
    } else if(size >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) {
        // UTF-16BE BOM
        bomSize = 2;
        encoding = "UTF-16BE";
    } else if(size >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        // UTF-8 BOM
        bomSize = 3;
        encoding = "UTF-8";
    } else {
        // ---------- 普通文本：用 uchardet 检测编码 ----------
        uchardet_t ud = uchardet_new();
        if(ud) {
            uchardet_handle_data(ud, reinterpret_cast<const char *>(raw),
                                 size);
            uchardet_data_end(ud);
            if(const char *detected = uchardet_get_charset(ud))
                encoding = detected;
            uchardet_delete(ud);
        }
        if(encoding == "SHIFT_JIS") {
            encoding = "cp932";
        } else if(encoding == "WINDOWS-1252") {
            encoding = "ASCII";
        } else if(encoding.find("MAC") != std::string::npos) {
            encoding = "cp932";
        }

        if(hasNonAsciiBytes(raw, size)) {
            if(encoding.empty() || encoding == "ASCII") {
                encoding = "cp932";
            } else if(encoding == "UTF-8") {
                if(!isValidUTF8(raw, size))
                    encoding = "cp932";
            } else if(encoding != "cp932" && encoding != "EUC-JP" &&
                       encoding != "ISO-2022-JP") {
                encoding = "cp932";
            }
        }
    }

    return encoding;
}

/*
 *  note: encryption of mode 0 or 1 ( simple crypt ) does never
 *  intend data pretection security.
 */
class tTVPTextReadStream : public iTJSTextReadStream {
    static constexpr size_t kMaxCompressedTextSize = 256ULL * 1024ULL * 1024ULL;

    std::unique_ptr<tTJSBinaryStream> _stream{};
    std::u16string _buffer; // 全部文本，UTF-16
    size_t _pos = 0; // 当前读取位置

public:
    tTVPTextReadStream(const ttstr &name, const ttstr &mode) {
        _stream.reset(TVPCreateStream(name, TJS_BS_READ));
        const ModeNumberResult offset_mode =
            parseModeNumberStrict(mode.c_str(), TJS_W('o'));
        if(!offset_mode.valid ||
           (offset_mode.present && !offset_mode.has_digits) ||
           (offset_mode.present &&
            offset_mode.value >
                static_cast<std::uint64_t>(std::numeric_limits<tjs_int64>::max())))
            TVPThrowExceptionMessage(TVPUnsupportedModeString, mode);
        const tjs_uint64 ofs =
            offset_mode.present ? offset_mode.value : static_cast<tjs_uint64>(0);
        const tjs_uint64 stream_size = _stream->GetSize();
        if(ofs > stream_size ||
           stream_size - ofs > std::numeric_limits<size_t>::max() ||
           stream_size - ofs > std::numeric_limits<tjs_uint>::max())
            TVPThrowExceptionMessage(TVPReadError, name);
        _stream->SetPosition(ofs);

        auto size = static_cast<size_t>(stream_size - ofs);
        std::vector<std::uint8_t> raw(size);
        _stream->ReadBuffer(raw.data(), size);

        if(size == 0) {
            _buffer.clear();
            return;
        }

        // ---------- 检查是否加密/压缩 ----------
        if(size >= 3 && raw[0] == 0xFE && raw[1] == 0xFE) {
            std::uint8_t m = raw[2];
            if(m == 0 || m == 1) {
                size_t hdr = 3;
                if(size >= 5 && raw[3] == 0xFF && raw[4] == 0xFE)
                    hdr = 5; // skip unencrypted UTF-16LE BOM
                else if(size >= 5 && raw[3] == 0xFE && raw[4] == 0xFF)
                    hdr = 5; // skip unencrypted UTF-16BE BOM
                size_t data_size = size - hdr;
                if(data_size & 1)
                    TVPThrowExceptionMessage(TVPUnsupportedCipherMode, name);
                size_t len = data_size / 2;
                _buffer.resize(len);
                for(size_t i = 0; i < len; i++) {
                    char16_t ch =
                        static_cast<char16_t>(raw[hdr + i * 2]) |
                        (static_cast<char16_t>(raw[hdr + i * 2 + 1]) << 8);
                    if(m == 0) {
                        if(ch >= 0x20)
                            ch ^= (((ch & 0xfe) << 8) ^ 1);
                    } else if(m == 1) {
                        ch =
                            ((ch & 0xaaaaaaaa) >> 1) | ((ch & 0x55555555) << 1);
                    }
                    _buffer[i] = ch;
                }
                return;
            }
            if(m == 2) {
                // 压缩流
                constexpr size_t headerSize = 3 + 2 + 16;
                if(size < headerSize)
                    TVPThrowExceptionMessage(TVPUnsupportedCipherMode, name);

                // 读压缩大小和解压大小
                std::uint8_t *ptr = raw.data() + 5;
                auto readU64LE = [](const std::uint8_t *data) {
                    std::uint64_t value = 0;
                    for(size_t i = 0; i < 8; ++i)
                        value |= static_cast<std::uint64_t>(data[i]) << (i * 8);
                    return value;
                };
                std::uint64_t compressed = readU64LE(ptr);
                ptr += 8;
                std::uint64_t uncompressed = readU64LE(ptr);
                ptr += 8;

                if(compressed > size - headerSize ||
                   compressed > kMaxCompressedTextSize ||
                   compressed > std::numeric_limits<size_t>::max() ||
                   uncompressed > std::numeric_limits<size_t>::max() ||
                   uncompressed > kMaxCompressedTextSize ||
                   uncompressed > std::numeric_limits<unsigned long>::max() ||
                   (uncompressed & 1))
                    TVPThrowExceptionMessage(TVPUnsupportedCipherMode, name);

                const size_t compressedSize = static_cast<size_t>(compressed);
                const size_t uncompressedSize =
                    static_cast<size_t>(uncompressed);
                std::vector<std::uint8_t> compBuf(compressedSize);
                memcpy(compBuf.data(), ptr, compressedSize);

                std::vector<std::uint8_t> uncompBuf(
                    uncompressedSize == 0 ? 1 : uncompressedSize);
                auto destLen = static_cast<unsigned long>(
                    uncompressedSize == 0 ? 1 : uncompressedSize);
                int ret = uncompress(uncompBuf.data(), &destLen, compBuf.data(),
                                     static_cast<unsigned long>(compressedSize));
                if(ret != Z_OK || destLen != uncompressed)
                    TVPThrowExceptionMessage(TVPUnsupportedCipherMode, name);

                // 解压得到 UTF-16 数据
                _buffer.resize(uncompressedSize / 2);
                for(size_t i = 0; i < _buffer.size(); ++i) {
                    _buffer[i] = static_cast<char16_t>(uncompBuf[i * 2]) |
                                 (static_cast<char16_t>(uncompBuf[i * 2 + 1])
                                  << 8);
                }
                return;
            }
            TVPThrowExceptionMessage(TVPUnsupportedCipherMode, name);
        }
        std::uint8_t bomSize = 0;
        std::string encoding = checkTextEncoding(raw.data(), size, bomSize);
        raw.erase(raw.begin(), raw.begin() + bomSize);
        size = raw.size();

        if(encoding.empty()) {
            std::lock_guard<std::mutex> lock(G_DefaultReadEncodingMutex);
            encoding = G_DefaultReadEncoding; // 默认回退
        }

        if(encoding == "ASCII") {
            _buffer.assign(raw.data(), raw.data() + size);
            return;
        }

        if(encoding == "UTF-8") {
            try {
                _buffer = boost::locale::conv::utf_to_utf<char16_t>(
                    reinterpret_cast<const char *>(raw.data()),
                    reinterpret_cast<const char *>(raw.data() + size));
            } catch(const std::exception &e) {
                spdlog::error("UTF-8 text conversion failed: {}", e.what());
                TVPThrowExceptionMessage(TJSNarrowToWideConversionError);
            } catch(...) {
                spdlog::error("UTF-8 text conversion failed");
                TVPThrowExceptionMessage(TJSNarrowToWideConversionError);
            }
            return;
        }

        if(encoding == "UTF-16" || encoding == "UTF-16LE" ||
           encoding == "UTF-16BE") {
            if(raw.size() & 1)
                TVPThrowExceptionMessage(TVPUnsupportedCipherMode, name);
            const bool bigEndian = encoding == "UTF-16BE";
            _buffer.resize(raw.size() / 2);
            for(size_t i = 0; i < _buffer.size(); ++i) {
                const size_t offset = i * 2;
                if(bigEndian)
                    _buffer[i] = (static_cast<char16_t>(raw[offset]) << 8) |
                                 static_cast<char16_t>(raw[offset + 1]);
                else
                    _buffer[i] = static_cast<char16_t>(raw[offset]) |
                                 (static_cast<char16_t>(raw[offset + 1]) << 8);
            }

            return;
        }

        if(encoding == "UTF-32" || encoding == "UTF-32LE" ||
           encoding == "UTF-32BE") {
            if(raw.size() & 3)
                TVPThrowExceptionMessage(TVPUnsupportedCipherMode, name);
            const bool bigEndian = encoding == "UTF-32BE";
            std::u32string codepoints(raw.size() / 4, U'\0');
            for(size_t i = 0; i < codepoints.size(); ++i) {
                const size_t offset = i * 4;
                if(bigEndian) {
                    codepoints[i] = (static_cast<char32_t>(raw[offset]) << 24) |
                                    (static_cast<char32_t>(raw[offset + 1]) << 16) |
                                    (static_cast<char32_t>(raw[offset + 2]) << 8) |
                                    static_cast<char32_t>(raw[offset + 3]);
                } else {
                    codepoints[i] = static_cast<char32_t>(raw[offset]) |
                                    (static_cast<char32_t>(raw[offset + 1]) << 8) |
                                    (static_cast<char32_t>(raw[offset + 2]) << 16) |
                                    (static_cast<char32_t>(raw[offset + 3]) << 24);
                }
                if(codepoints[i] > 0x10ffff ||
                   (codepoints[i] >= 0xd800 && codepoints[i] <= 0xdfff))
                    TVPThrowExceptionMessage(TJSNarrowToWideConversionError);
            }
            try {
                _buffer = boost::locale::conv::utf_to_utf<char16_t>(codepoints);
            } catch(const std::exception &e) {
                spdlog::error("UTF-32 text conversion failed: {}", e.what());
                TVPThrowExceptionMessage(TJSNarrowToWideConversionError);
            } catch(...) {
                spdlog::error("UTF-32 text conversion failed");
                TVPThrowExceptionMessage(TJSNarrowToWideConversionError);
            }
            return;
        }

        // 其他文本字符
        try {
            std::wstring wide = boost::locale::conv::to_utf<wchar_t>(
                reinterpret_cast<const char *>(raw.data()),
                reinterpret_cast<const char *>(raw.data() + raw.size()),
                encoding);
            _buffer = boost::locale::conv::utf_to_utf<char16_t>(wide);
        } catch(const std::exception &e) {
            spdlog::error("text conversion failed: {}", e.what());
            TVPThrowExceptionMessage(TJSNarrowToWideConversionError);
        } catch(...) {
            spdlog::error("text conversion failed");
            TVPThrowExceptionMessage(TJSNarrowToWideConversionError);
        }
    }

    ~tTVPTextReadStream() override = default;

    tjs_uint Read(tTJSString &targ, tjs_uint size) override {
        static_assert(sizeof(tjs_char) == sizeof(char16_t),
                      "Char size mismatch");
        if(_pos >= _buffer.size()) {
            targ.Clear();
            return 0;
        }
        size_t remain = _buffer.size() - _pos;
        size_t n = size ? std::min<size_t>(size, remain)
                        : std::min<size_t>(remain,
                                           std::numeric_limits<tjs_uint>::max());
        tjs_char *buf = targ.AllocBuffer(n);
        std::copy_n(_buffer.data() + _pos, n, buf);
        buf[n] = 0;
        _pos += n;
        targ.FixLen();
        return n;
    }

    void Destruct() override { delete this; }
};


class tTVPTextWriteStream : public iTJSTextWriteStream {
    // TODO: 32bit wchar_t support

    static constexpr size_t COMPRESSION_BUFFER_SIZE = 1024 * 1024;

    std::unique_ptr<tTJSBinaryStream> _stream{};
    tjs_int _cryptMode{};
    // -1 for no-crypt
    // 0: (unused)	(old buggy crypt mode)
    // 1: simple crypt
    // 2: complessed
    int _compressionLevel{ Z_DEFAULT_COMPRESSION }; // compression level of zlib

    std::unique_ptr<z_stream_s> _zStream{};
    tjs_uint64 _compressionSizePosition{ 0 };
    std::vector<Bytef> _compressionBuffer =
        std::vector<Bytef>(COMPRESSION_BUFFER_SIZE);
    bool _compressionFailed{ false };
    bool _compressionFinalized{ false };

public:
    tTVPTextWriteStream(const ttstr &name, const ttstr &mode) {
        // mode supports following modes:
        // dN: deflate(compress) at mode N ( currently not implemented
        // ) cN: write in cipher at mode N ( currently n is ignored )
        // zN: write with compress at mode N ( N is compression level
        // ) oN: write from binary offset N (in bytes)

        // check c/z mode
        _cryptMode = -1;
        const ModeNumberResult cipher_mode =
            parseModeNumberStrict(mode.c_str(), TJS_W('c'));
        const ModeNumberResult compression_mode =
            parseModeNumberStrict(mode.c_str(), TJS_W('z'));
        const ModeNumberResult offset_mode =
            parseModeNumberStrict(mode.c_str(), TJS_W('o'));
        if(!cipher_mode.valid || !compression_mode.valid ||
           !offset_mode.valid ||
           (offset_mode.present && !offset_mode.has_digits) ||
           (cipher_mode.present && compression_mode.present))
            TVPThrowExceptionMessage(TVPUnsupportedModeString,
                                     mode);

        if(cipher_mode.present) {
            const std::uint64_t mode_value =
                cipher_mode.has_digits ? cipher_mode.value : 1;
            if(mode_value != 1)
                TVPThrowExceptionMessage(TVPUnsupportedModeString, mode);
            _cryptMode = 1;
        }

        if(compression_mode.present) {
            if(compression_mode.has_digits && compression_mode.value > 9)
                TVPThrowExceptionMessage(TVPUnsupportedModeString, mode);
            const int level = compression_mode.has_digits
                                  ? static_cast<int>(compression_mode.value)
                                  : Z_DEFAULT_COMPRESSION;
            _compressionLevel = level;
            _cryptMode = 2;
        }

        // check o mode
        if(offset_mode.present &&
           offset_mode.value >
               static_cast<std::uint64_t>(std::numeric_limits<tjs_int64>::max()))
            TVPThrowExceptionMessage(TVPUnsupportedModeString, mode);
        if(offset_mode.present && offset_mode.value != 0) {
            _stream.reset(TVPCreateStream(name, TJS_BS_UPDATE));
            _stream->SetPosition(offset_mode.value);
        } else {
            _stream.reset(TVPCreateStream(name, TJS_BS_WRITE));
        }

        if(_cryptMode == 1 || _cryptMode == 2) {
            // simple crypt or compressed
            tjs_uint8 crypt_mode_sig[4];
            crypt_mode_sig[0] = crypt_mode_sig[1] = 0xfe;
            crypt_mode_sig[2] = static_cast<tjs_uint8>(_cryptMode);
            crypt_mode_sig[3] = 0;
            _stream->WriteBuffer(crypt_mode_sig, 3);
        }

        // now output text stream will write unicode texts
        static tjs_uint8 bommark[2] = { 0xff, 0xfe };
        _stream->WriteBuffer(bommark, 2);

        if(_cryptMode == 2) {
            // allocate and initialize zlib straem
            _zStream.reset(new z_stream_s());
            _zStream->zalloc = Z_NULL;
            _zStream->zfree = Z_NULL;
            _zStream->opaque = Z_NULL;
            if(deflateInit(_zStream.get(), _compressionLevel) != Z_OK) {
                _compressionFailed = true;
                TVPThrowExceptionMessage(TVPCompressionFailed);
            }

            _zStream->next_in = nullptr;
            _zStream->avail_in = 0;
            _zStream->next_out = _compressionBuffer.data();
            _zStream->avail_out = COMPRESSION_BUFFER_SIZE;

            // Compression Size (write dummy)
            _compressionSizePosition = _stream->GetPosition();
            WriteI64LE(0);
            WriteI64LE(0);
        }
    }

    void WriteCompressedBytes(size_t size) {
        if(size == 0)
            return;
        try {
            _stream->WriteBuffer(_compressionBuffer.data(), size);
        } catch(...) {
            _compressionFailed = true;
            throw;
        }
    }

    void FinalizeCompression(bool report_failure) {
        if(_compressionFinalized) {
            if(report_failure && _compressionFailed)
                TVPThrowExceptionMessage(TVPCompressionFailed);
            return;
        }
        _compressionFinalized = true;

        if(_cryptMode == 2 && !_compressionFailed) {
            try {
                int result = 0;
                do {
                    result = deflate(_zStream.get(), Z_FINISH);
                    if(result != Z_OK && result != Z_STREAM_END) {
                        _compressionFailed = true;
                        TVPThrowExceptionMessage(TVPCompressionFailed);
                    }
                    WriteCompressedBytes(COMPRESSION_BUFFER_SIZE -
                                        _zStream->avail_out);
                    _zStream->next_out = _compressionBuffer.data();
                    _zStream->avail_out = COMPRESSION_BUFFER_SIZE;
                } while(result != Z_STREAM_END);

                // Rewind and fill in the compressed and uncompressed sizes.
                _stream->SetPosition(_compressionSizePosition);
                WriteI64LE(_zStream->total_out);
                WriteI64LE(_zStream->total_in);
            } catch(...) {
                _compressionFailed = true;
                spdlog::error("Failed to finalize compressed text stream");
            }
        }

        if(_zStream) {
            if(deflateEnd(_zStream.get()) != Z_OK)
                _compressionFailed = true;
            _zStream.reset();
        }

        if(report_failure && _compressionFailed)
            TVPThrowExceptionMessage(TVPCompressionFailed);
    }

    ~tTVPTextWriteStream() override {
        try {
            FinalizeCompression(false);
        } catch(...) {
            // Destructors cannot report an exception. Destruct() performs the
            // observable finalization used by all normal callers.
        }
    }

    void WriteI64LE(tjs_uint64 v) {
        // write 64bit little endian value to the file.
        tjs_uint8 buf[8];
        for(int i = 0; i < 8; i++) {
            buf[i] = static_cast<tjs_uint8>(v >> (i * 8));
        }
        _stream->WriteBuffer(buf, 8);
    }

    void Write(const ttstr &targ) override {
        tjs_int len = targ.GetLen();
        auto buf = std::make_unique<tjs_uint16[]>(len + 1);
        const tjs_char *src = targ.c_str();
        tjs_int i;
        for(i = 0; i < len; i++) {
            buf[i] = src[i];
        }
        buf[i] = 0;

        if(_cryptMode == 1) {
            // simple crypt
            if(tjs_uint16 *p = buf.get()) {
                while(*p) {
                    tjs_char ch = *p;
                    ch = (ch & 0xaaaaaaaa) >> 1 | (ch & 0x55555555) << 1;
                    *p = ch;
                    p++;
                }
            }

            WriteRawData(buf.get(), len * sizeof(tjs_uint16));
        } else {
            WriteRawData(buf.get(), len * sizeof(tjs_uint16));
        }
    }

    void WriteRawData(void *ptr, size_t size) {
        if(_cryptMode == 2) {
            if(_compressionFinalized || _compressionFailed || !_zStream) {
                _compressionFailed = true;
                TVPThrowExceptionMessage(TVPCompressionFailed);
            }
            // compressed with zlib stream.
            auto *input = static_cast<Bytef *>(ptr);
            size_t remaining = size;
            while(remaining > 0) {
                const uInt chunk = static_cast<uInt>(std::min<size_t>(
                    remaining, std::numeric_limits<uInt>::max()));
                _zStream->next_in = input;
                _zStream->avail_in = chunk;
                while(_zStream->avail_in > 0) {
                    int result = deflate(_zStream.get(), Z_NO_FLUSH);
                    if(result != Z_OK) {
                        _compressionFailed = true;
                        TVPThrowExceptionMessage(TVPCompressionFailed);
                    }
                    if(_zStream->avail_out == 0) {
                        WriteCompressedBytes(COMPRESSION_BUFFER_SIZE);
                        _zStream->next_out = _compressionBuffer.data();
                        _zStream->avail_out = COMPRESSION_BUFFER_SIZE;
                    }
                }
                const size_t consumed = chunk - _zStream->avail_in;
                input += consumed;
                remaining -= consumed;
                if(consumed == 0) {
                    _compressionFailed = true;
                    TVPThrowExceptionMessage(TVPCompressionFailed);
                }
            }
        } else {
            _stream->WriteBuffer(ptr, size); // write directly
        }
    }

    void Destruct() override {
        try {
            FinalizeCompression(true);
        } catch(...) {
            delete this;
            throw;
        }
        delete this;
    }
};

iTJSTextReadStream *TVPCreateTextStreamForRead(const ttstr &name,
                                               const ttstr &mode) {
    return new tTVPTextReadStream(name, mode);
}

iTJSTextWriteStream *TVPCreateTextStreamForWrite(const ttstr &name,
                                                 const ttstr &mode) {
    return new tTVPTextWriteStream(name, mode);
}

//---------------------------------------------------------------------------
void TVPSetDefaultReadEncoding(const ttstr &encoding) {
    std::lock_guard<std::mutex> lock(G_DefaultReadEncodingMutex);
    ttstr codestr = encoding;
    codestr.ToLowerCase();
    if(codestr == TJS_W("sjis") || codestr == TJS_W("shiftjis") ||
       codestr == TJS_W("shift_jis") || codestr == TJS_W("shift-jis")) {
        G_DefaultReadEncoding = "cp932";
    } else if(codestr == TJS_W("utf8") || codestr == TJS_W("utf-8")) {
        G_DefaultReadEncoding = "UTF-8";
    } else {
        G_DefaultReadEncoding = encoding.AsStdString();
    }
}

//---------------------------------------------------------------------------
const tjs_char *TVPGetDefaultReadEncoding() {
    static thread_local ttstr value;
    std::lock_guard<std::mutex> lock(G_DefaultReadEncodingMutex);
    value = ttstr{ G_DefaultReadEncoding };
    return value.c_str();
}
