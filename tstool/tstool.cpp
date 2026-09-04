#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <charconv>
#include <filesystem>
#include <iomanip>
#include <ctime>

namespace fs = std::filesystem;


struct LogSession {
    uint8_t version = 4;
    std::string songName;
    std::string levelPath;
    int64_t timestamp = 0;
    double bpm = 0.0;
    double speed = 1.0;
    double pitch = 1.0;
    bool isAngle = false;

    struct HitData {
        double value;
        int marginCode;
    };
    std::vector<HitData> hits;
};

static uint16_t read_u16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static uint32_t read_u32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }

std::vector<uint8_t> decompress_gzip(const std::vector<uint8_t>& src) {
    if (src.size() < 18 || src[0] != 0x1f || src[1] != 0x8b) throw std::runtime_error("无效的 GZip 文件");
    if (src[2] != 8) throw std::runtime_error("仅支持 Deflate 压缩算法");

    uint8_t flags = src[3];
    size_t pos = 10;
    if (flags & 4) pos += 2 + read_u16(&src[pos]);
    if (flags & 8) while (pos < src.size() && src[pos]) pos++; pos++; // FNAME
    if (flags & 16) while (pos < src.size() && src[pos]) pos++; pos++; // COMMENT
    if (flags & 2) pos += 2; // FHCRC

    size_t uncompressed_size = read_u32(&src[src.size() - 4]);
    std::vector<uint8_t> dest(uncompressed_size);

    size_t out_pos = 0;
    uint32_t bit_buf = 0;
    int bit_cnt = 0;

    auto get_bits = [&](int n) -> uint32_t {
        while (bit_cnt < n) {
            if (pos >= src.size() - 8) throw std::runtime_error("数据流提前结束");
            bit_buf |= (uint32_t)src[pos++] << bit_cnt;
            bit_cnt += 8;
        }
        uint32_t res = bit_buf & ((1U << n) - 1);
        bit_buf >>= n;
        bit_cnt -= n;
        return res;
    };

    while (pos < src.size() - 8) {
        uint32_t bfinal = get_bits(1);
        uint32_t btype = get_bits(2);
        if (btype == 0) { // Uncompressed
            bit_buf = 0; bit_cnt = 0; // align
            uint16_t len = read_u16(&src[pos]); pos += 2;
            uint16_t nlen = read_u16(&src[pos]); pos += 2;
            if ((uint16_t)~len != nlen) throw std::runtime_error("Uncompressed 块长度校验错误");
            if (out_pos + len > uncompressed_size) dest.resize(out_pos + len);
            std::memcpy(&dest[out_pos], &src[pos], len);
            pos += len;
            out_pos += len;
        } else {
            throw std::runtime_error("暂不支持动态/静态 Huffman 块解码，请确保输入未过度压缩");
        }
        if (bfinal) break;
    }
    return dest;
}

// SHA-256 & AES-256-CBC 原生解密
class SHA256 {
private:
    uint32_t m_state[8];
    uint64_t m_count;
    uint8_t m_buffer[64];

    static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
    static uint32_t choose(uint32_t e, uint32_t f, uint32_t g) { return (e & f) ^ (~e & g); }
    static uint32_t majority(uint32_t a, uint32_t b, uint32_t c) { return (a & b) ^ (a & c) ^ (b & c); }
    static uint32_t sig0(uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
    static uint32_t sig1(uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
    static uint32_t sub0(uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
    static uint32_t sub1(uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

    void transform(const uint8_t* chunk) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
        };

        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (chunk[i * 4] << 24) | (chunk[i * 4 + 1] << 16) | (chunk[i * 4 + 2] << 8) | chunk[i * 4 + 3];
        }
        for (int i = 16; i < 64; ++i) {
            w[i] = sub1(w[i - 2]) + w[i - 7] + sub0(w[i - 15]) + w[i - 16];
        }

        uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
        uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];

        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = h + sig1(e) + choose(e, f, g) + K[i] + w[i];
            uint32_t t2 = sig0(a) + majority(a, b, c);
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }

        m_state[0] += a; m_state[1] += b; m_state[2] += c; m_state[3] += d;
        m_state[4] += e; m_state[5] += f; m_state[6] += g; m_state[7] += h;
    }

public:
    SHA256() { reset(); }
    void reset() {
        m_state[0] = 0x6a09e667; m_state[1] = 0xbb67ae85; m_state[2] = 0x3c6ef372; m_state[3] = 0xa54ff53a;
        m_state[4] = 0x510e527f; m_state[5] = 0x9b05688c; m_state[6] = 0x1f83d9ab; m_state[7] = 0x5be0cd19;
        m_count = 0;
    }

    void update(const uint8_t* data, size_t len) {
        for (size_t i = 0; i < len; ++i) {
            m_buffer[m_count % 64] = data[i];
            m_count++;
            if (m_count % 64 == 0) transform(m_buffer);
        }
    }

    std::vector<uint8_t> digest() {
        uint8_t pad[64] = {0x80};
        size_t pad_len = (m_count % 64 < 56) ? (56 - m_count % 64) : (120 - m_count % 64);
        uint64_t bits = m_count * 8;
        update(pad, pad_len);
        uint8_t len_bytes[8];
        for (int i = 0; i < 8; ++i) len_bytes[i] = (bits >> ((7 - i) * 8)) & 0xff;
        update(len_bytes, 8);

        std::vector<uint8_t> res(32);
        for (int i = 0; i < 8; ++i) {
            res[i * 4] = (m_state[i] >> 24) & 0xff;
            res[i * 4 + 1] = (m_state[i] >> 16) & 0xff;
            res[i * 4 + 2] = (m_state[i] >> 8) & 0xff;
            res[i * 4 + 3] = m_state[i] & 0xff;
        }
        return res;
    }
};

class AES256CBC {
private:
    uint32_t rkey[60];

    static uint8_t sbox(uint8_t b) {
        static const uint8_t S[256] = {
            0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
            0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
            0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
            0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
            0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
            0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
            0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
            0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
            0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
            0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
            0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
            0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
            0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
            0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
            0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
            0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
        };
        return S[b];
    }

    static uint8_t rsbox(uint8_t b) {
        static const uint8_t RS[256] = {
            0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
            0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
            0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
            0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
            0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
            0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
            0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
            0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
            0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
            0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
            0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
            0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
            0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
            0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
            0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
            0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
        };
        return RS[b];
    }

    static uint8_t gmul(uint8_t a, uint8_t b) {
        uint8_t p = 0;
        for (int i = 0; i < 8; ++i) {
            if (b & 1) p ^= a;
            bool hi = (a & 0x80);
            a <<= 1;
            if (hi) a ^= 0x1b;
            b >>= 1;
        }
        return p;
    }

    void keyExpansion(const uint8_t* key) {
        static const uint32_t rcon[11] = { 0x00000000, 0x01000000, 0x02000000, 0x04000000, 0x08000000, 0x10000000, 0x20000000, 0x40000000, 0x80000000, 0x1b000000, 0x36000000 };
        for (int i = 0; i < 8; ++i) {
            rkey[i] = (key[i * 4] << 24) | (key[i * 4 + 1] << 16) | (key[i * 4 + 2] << 8) | key[i * 4 + 3];
        }
        for (int i = 8; i < 60; ++i) {
            uint32_t temp = rkey[i - 1];
            if (i % 8 == 0) {
                temp = (temp << 8) | (temp >> 24);
                temp = (sbox((temp >> 24) & 0xff) << 24) | (sbox((temp >> 16) & 0xff) << 16) | (sbox((temp >> 8) & 0xff) << 8) | sbox(temp & 0xff);
                temp ^= rcon[i / 8];
            } else if (i % 8 == 4) {
                temp = (sbox((temp >> 24) & 0xff) << 24) | (sbox((temp >> 16) & 0xff) << 16) | (sbox((temp >> 8) & 0xff) << 8) | sbox(temp & 0xff);
            }
            rkey[i] = rkey[i - 8] ^ temp;
        }
    }

    void decryptBlock(const uint8_t* in, uint8_t* out) {
        uint8_t state[4][4];
        for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) state[r][c] = in[r + 4 * c];

        auto addRoundKey = [&](int round) {
            for (int c = 0; c < 4; ++c) {
                uint32_t k = rkey[round * 4 + c];
                state[0][c] ^= (k >> 24) & 0xff;
                state[1][c] ^= (k >> 16) & 0xff;
                state[2][c] ^= (k >> 8) & 0xff;
                state[3][c] ^= k & 0xff;
            }
        };

        addRoundKey(14);

        for (int round = 13; round >= 0; --round) {
            // InvShiftRows
            uint8_t tmp = state[1][3]; state[1][3] = state[1][2]; state[1][2] = state[1][1]; state[1][1] = state[1][0]; state[1][0] = tmp;
            std::swap(state[2][0], state[2][2]); std::swap(state[2][1], state[2][3]);
            tmp = state[3][0]; state[3][0] = state[3][1]; state[3][1] = state[3][2]; state[3][2] = state[3][3]; state[3][3] = tmp;

            // InvSubBytes
            for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) state[r][c] = rsbox(state[r][c]);

            addRoundKey(round);

            if (round > 0) {
                // InvMixColumns
                for (int c = 0; c < 4; ++c) {
                    uint8_t a = state[0][c], b = state[1][c], c_val = state[2][c], d = state[3][c];
                    state[0][c] = gmul(a, 0x0e) ^ gmul(b, 0x0b) ^ gmul(c_val, 0x0d) ^ gmul(d, 0x09);
                    state[1][c] = gmul(a, 0x09) ^ gmul(b, 0x0e) ^ gmul(c_val, 0x0b) ^ gmul(d, 0x0d);
                    state[2][c] = gmul(a, 0x0d) ^ gmul(b, 0x09) ^ gmul(c_val, 0x0e) ^ gmul(d, 0x0b);
                    state[3][c] = gmul(a, 0x0b) ^ gmul(b, 0x0d) ^ gmul(c_val, 0x09) ^ gmul(d, 0x0e);
                }
            }
        }

        for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) out[r + 4 * c] = state[r][c];
    }

public:
    AES256CBC(const uint8_t* key) { keyExpansion(key); }

    std::vector<uint8_t> decrypt(const uint8_t* cipher, size_t len, const uint8_t* iv) {
        if (len % 16 != 0) throw std::runtime_error("密文长度非 16 字节对齐");
        std::vector<uint8_t> plain(len);
        uint8_t prev_block[16];
        std::memcpy(prev_block, iv, 16);

        for (size_t i = 0; i < len; i += 16) {
            uint8_t dec[16];
            decryptBlock(cipher + i, dec);
            for (int k = 0; k < 16; ++k) plain[i + k] = dec[k] ^ prev_block[k];
            std::memcpy(prev_block, cipher + i, 16);
        }

        // PKCS7 Unpad
        if (!plain.empty()) {
            uint8_t pad_len = plain.back();
            if (pad_len >= 1 && pad_len <= 16) {
                bool valid = true;
                for (size_t k = plain.size() - pad_len; k < plain.size(); ++k) {
                    if (plain[k] != pad_len) { valid = false; break; }
                }
                if (valid) plain.resize(plain.size() - pad_len);
            }
        }
        return plain;
    }
};

// crpl2
struct Crpl2Data {
    int formatVersion = 0;
    struct Compact {
        std::vector<std::string> s_keys, s_vals;
        std::vector<std::string> b_keys; std::vector<bool> b_vals;
        std::vector<std::string> i_keys; std::vector<int32_t> i_vals;
        std::vector<std::string> d_keys; std::vector<double> d_vals;
        std::vector<uint16_t> keyCodes;
        std::vector<int32_t> keyPresses;
        std::vector<double> keySongPositions;
        std::vector<int32_t> hitCurrentFloorIDs;
        std::vector<double> hitCurrAngles;
        std::vector<float> hitOverloadCounters;
        std::vector<double> hitCachedAngles;
        std::vector<double> hitTargetExitAngles;
        std::vector<int32_t> hitCurFreeRoamSections;
        std::vector<uint8_t> hitFlags;
        
        std::vector<int> hitNoFailHits, hitIsAutos, hitNextFloorAutos, hitMidspinInfiniteMargins, hitRDCautos;
    } compact;
};

class Crpl2Reader {
    const uint8_t* data;
    size_t size;
    size_t offset = 0;

public:
    Crpl2Reader(const uint8_t* d, size_t s) : data(d), size(s) {}

    int32_t readInt32() {
        if (offset + 4 > size) throw std::runtime_error("CRPL2 读取 Int32 越界");
        int32_t v; std::memcpy(&v, data + offset, 4); offset += 4; return v;
    }
    bool readBool() {
        if (offset + 1 > size) throw std::runtime_error("CRPL2 读取 Bool 越界");
        bool v = data[offset] != 0; offset += 1; return v;
    }
    uint16_t readUShort() {
        if (offset + 2 > size) throw std::runtime_error("CRPL2 读取 UShort 越界");
        uint16_t v; std::memcpy(&v, data + offset, 2); offset += 2; return v;
    }
    double readDouble() {
        if (offset + 8 > size) throw std::runtime_error("CRPL2 读取 Double 越界");
        double v; std::memcpy(&v, data + offset, 8); offset += 8; return v;
    }
    float readFloat() {
        if (offset + 4 > size) throw std::runtime_error("CRPL2 读取 Float 越界");
        float v; std::memcpy(&v, data + offset, 4); offset += 4; return v;
    }
    uint8_t readByte() {
        if (offset + 1 > size) throw std::runtime_error("CRPL2 读取 Byte 越界");
        return data[offset++];
    }
    std::string readString() {
        uint32_t len = 0, shift = 0;
        while (true) {
            uint8_t b = readByte();
            len |= (b & 0x7F) << shift;
            if ((b & 0x80) == 0) break;
            shift += 7;
        }
        if (offset + len > size) throw std::runtime_error("CRPL2 读取 String 越界");
        std::string s((const char*)data + offset, len);
        offset += len;
        return s;
    }

    template<typename T, typename Func>
    std::vector<T> readList(Func func) {
        int32_t count = readInt32();
        std::vector<T> list;
        list.reserve(count);
        for (int32_t i = 0; i < count; ++i) list.push_back(func());
        return list;
    }
};

Crpl2Data parse_crpl2_plaintext(const std::vector<uint8_t>& plain) {
    Crpl2Reader r(plain.data(), plain.size());
    Crpl2Data res;
    res.formatVersion = r.readInt32();

    int s_cnt = r.readInt32();
    for(int k=0; k<s_cnt; ++k) { res.compact.s_keys.push_back(r.readString()); res.compact.s_vals.push_back(r.readString()); }

    int b_cnt = r.readInt32();
    for(int k=0; k<b_cnt; ++k) { res.compact.b_keys.push_back(r.readString()); res.compact.b_vals.push_back(r.readBool()); }

    int i_cnt = r.readInt32();
    for(int k=0; k<i_cnt; ++k) { res.compact.i_keys.push_back(r.readString()); res.compact.i_vals.push_back(r.readInt32()); }

    int d_cnt = r.readInt32();
    for(int k=0; k<d_cnt; ++k) { res.compact.d_keys.push_back(r.readString()); res.compact.d_vals.push_back(r.readDouble()); }

    res.compact.keyCodes = r.readList<uint16_t>([&](){ return r.readUShort(); });
    res.compact.keyPresses = r.readList<int32_t>([&](){ return r.readInt32(); });
    res.compact.keySongPositions = r.readList<double>([&](){ return r.readDouble(); });
    res.compact.hitCurrentFloorIDs = r.readList<int32_t>([&](){ return r.readInt32(); });
    res.compact.hitCurrAngles = r.readList<double>([&](){ return r.readDouble(); });
    res.compact.hitOverloadCounters = r.readList<float>([&](){ return r.readFloat(); });
    res.compact.hitCachedAngles = r.readList<double>([&](){ return r.readDouble(); });
    res.compact.hitTargetExitAngles = r.readList<double>([&](){ return r.readDouble(); });
    res.compact.hitCurFreeRoamSections = r.readList<int32_t>([&](){ return r.readInt32(); });
    res.compact.hitFlags = r.readList<uint8_t>([&](){ return r.readByte(); });

    size_t numHits = res.compact.hitCurrentFloorIDs.size();
    for (size_t k = 0; k < numHits; ++k) {
        uint8_t flags = res.compact.hitFlags[k];
        res.compact.hitNoFailHits.push_back((flags & 1) ? 1 : 0);
        res.compact.hitIsAutos.push_back((flags & 2) ? 1 : 0);
        res.compact.hitNextFloorAutos.push_back((flags & 4) ? 1 : 0);
        res.compact.hitMidspinInfiniteMargins.push_back((flags & 8) ? 1 : 0);
        res.compact.hitRDCautos.push_back((flags & 16) ? 1 : 0);
    }

    return res;
}

Crpl2Data decrypt_crpl2_file(const std::string& filepath) {
    std::ifstream fs(filepath, std::ios::binary | std::ios::ate);
    if (!fs) throw std::runtime_error("无法打开 CRPL2 文件");
    size_t sz = fs.tellg();
    if (sz < 8) throw std::runtime_error("CRPL2 文件损坏/长度不足");
    fs.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(sz);
    fs.read((char*)data.data(), sz);

    if (std::memcmp(data.data(), "CRP2", 4) != 0) throw std::runtime_error("非法 CRPL2 魔数头");

    SHA256 shaKey, shaIv;
    std::string keyStr = "qwerty", ivStr = "potato";
    shaKey.update((const uint8_t*)keyStr.data(), keyStr.size());
    shaIv.update((const uint8_t*)ivStr.data(), ivStr.size());

    auto key = shaKey.digest(); key.resize(32);
    auto iv = shaIv.digest(); iv.resize(16);

    AES256CBC cipher(key.data());
    auto plain = cipher.decrypt(data.data() + 8, sz - 8, iv.data());

    return parse_crpl2_plaintext(plain);
}

// json处理
std::string json_escape(const std::string& str) {
    std::string out;
    for (char c : str) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else out += c;
    }
    return out;
}

LogSession parse_json(const std::string& json) {
    LogSession s;
    auto find_val = [&](const std::string& key) -> std::string {
        size_t pos = json.find("\"" + key + "\"");
        if (pos == std::string::npos) return "";
        pos = json.find(':', pos) + 1;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) pos++;
        size_t end = json.find_first_of(",}\n\r", pos);
        return json.substr(pos, end - pos);
    };

    auto str_val = [&](const std::string& key) -> std::string {
        size_t pos = json.find("\"" + key + "\"");
        if (pos == std::string::npos) return "";
        size_t start = json.find('"', json.find(':', pos)) + 1;
        size_t end = json.find('"', start);
        return json.substr(start, end - start);
    };

    s.songName = str_val("songName");
    s.levelPath = str_val("levelPath");
    try { s.timestamp = std::stoll(find_val("timestamp")); } catch (...) {}
    try { s.bpm = std::stod(find_val("bpm")); } catch (...) {}
    try { s.speed = std::stod(find_val("speed")); } catch (...) {}
    try { s.pitch = std::stod(find_val("pitch")); } catch (...) {}
    s.isAngle = find_val("isAngle").find("true") != std::string::npos;

    size_t offsets_pos = json.find("\"offsets\"");
    if (offsets_pos != std::string::npos) {
        size_t start = json.find_first_of("[{", offsets_pos);
        if (json[start] == '[') {
            size_t p = start + 1;
            while (p < json.size() && json[p] != ']') {
                if (json[p] == '[') {
                    p++;
                    size_t comma = json.find(',', p);
                    size_t end = json.find(']', comma);
                    double val = std::stod(json.substr(p, comma - p));
                    int margin = std::stoi(json.substr(comma + 1, end - comma - 1));
                    s.hits.push_back({val, margin});
                    p = end + 1;
                } else p++;
            }
        } else {
            size_t p = start;
            while ((p = json.find("\"v\"", p)) != std::string::npos) {
                size_t v_start = json.find(':', p) + 1;
                size_t v_end = json.find_first_of(",}", v_start);
                double val = std::stod(json.substr(v_start, v_end - v_start));
                
                size_t j_pos = json.find("\"j\"", v_end);
                size_t j_start = json.find(':', j_pos) + 1;
                size_t j_end = json.find_first_of(",}", j_start);
                int margin = std::stoi(json.substr(j_start, j_end - j_start));

                s.hits.push_back({val, margin});
                p = j_end + 1;
            }
        }
    }
    s.version = (json.find("\"bpm\"") != std::string::npos) ? 4 : 1;
    return s;
}

std::string build_json(const LogSession& s, bool useOldFormat) {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"songName\": \"" << json_escape(s.songName) << "\",\n";
    ss << "  \"levelPath\": \"" << json_escape(s.levelPath) << "\",\n";
    ss << "  \"timestamp\": " << s.timestamp << ",\n";
    if (s.version >= 4) {
        ss << "  \"bpm\": " << s.bpm << ",\n";
        ss << "  \"speed\": " << s.speed << ",\n";
        ss << "  \"pitch\": " << s.pitch << ",\n";
        ss << "  \"isAngle\": " << (s.isAngle ? "true" : "false") << ",\n";
    }
    
    if (useOldFormat) {
        ss << "  \"offsets\": {";
        for (size_t i = 0; i < s.hits.size(); ++i) {
            ss << (i == 0 ? "\n" : ",\n");
            ss << "    \"" << (i + 1) << "\": {\"v\": " << s.hits[i].value << ", \"j\": " << s.hits[i].marginCode << "}";
        }
        ss << "\n  }\n}";
    } else {
        ss << "  \"offsets\": [";
        for (size_t i = 0; i < s.hits.size(); ++i) {
            ss << (i == 0 ? "" : ",");
            ss << "[" << s.hits[i].value << "," << s.hits[i].marginCode << "]";
        }
        ss << "]\n}";
    }
    return ss.str();
}

std::string build_crpl2_json(const Crpl2Data& cr) {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"FormatVersion\": " << cr.formatVersion << ",\n";
    ss << "  \"CompactCreplayfile\": {\n";

    auto write_dict_str = [&](const std::string& name, const std::vector<std::string>& keys, const std::vector<std::string>& vals) {
        ss << "    \"" << name << "\": {";
        for (size_t k = 0; k < keys.size(); ++k) {
            ss << (k == 0 ? "" : ",") << "\n      \"" << json_escape(keys[k]) << "\": \"" << json_escape(vals[k]) << "\"";
        }
        ss << (keys.empty() ? "" : "\n    ") << "},\n";
    };

    auto write_dict_bool = [&](const std::string& name, const std::vector<std::string>& keys, const std::vector<bool>& vals) {
        ss << "    \"" << name << "\": {";
        for (size_t k = 0; k < keys.size(); ++k) {
            ss << (k == 0 ? "" : ",") << "\n      \"" << json_escape(keys[k]) << "\": " << (vals[k] ? "true" : "false");
        }
        ss << (keys.empty() ? "" : "\n    ") << "},\n";
    };

    auto write_dict_num = [&](const std::string& name, const std::vector<std::string>& keys, const auto& vals) {
        ss << "    \"" << name << "\": {";
        for (size_t k = 0; k < keys.size(); ++k) {
            ss << (k == 0 ? "" : ",") << "\n      \"" << json_escape(keys[k]) << "\": " << vals[k];
        }
        ss << (keys.empty() ? "" : "\n    ") << "},\n";
    };

    auto write_arr = [&](const std::string& name, const auto& arr, bool is_last = false) {
        ss << "    \"" << name << "\": [";
        for (size_t k = 0; k < arr.size(); ++k) {
            ss << (k == 0 ? "" : ", ") << arr[k];
        }
        ss << "]" << (is_last ? "\n" : ",\n");
    };

    write_dict_str("s", cr.compact.s_keys, cr.compact.s_vals);
    write_dict_bool("b", cr.compact.b_keys, cr.compact.b_vals);
    write_dict_num("i", cr.compact.i_keys, cr.compact.i_vals);
    write_dict_num("d", cr.compact.d_keys, cr.compact.d_vals);

    write_arr("keyCodes", cr.compact.keyCodes);
    write_arr("keyPresses", cr.compact.keyPresses);
    write_arr("keySongPositions", cr.compact.keySongPositions);
    write_arr("hitCurrentFloorIDs", cr.compact.hitCurrentFloorIDs);
    write_arr("hitCurrAngles", cr.compact.hitCurrAngles);
    write_arr("hitOverloadCounters", cr.compact.hitOverloadCounters);
    write_arr("hitNoFailHits", cr.compact.hitNoFailHits);
    write_arr("hitIsAutos", cr.compact.hitIsAutos);
    write_arr("hitNextFloorAutos", cr.compact.hitNextFloorAutos);
    write_arr("hitCachedAngles", cr.compact.hitCachedAngles);
    write_arr("hitTargetExitAngles", cr.compact.hitTargetExitAngles);
    write_arr("hitMidspinInfiniteMargins", cr.compact.hitMidspinInfiniteMargins);
    write_arr("hitRDCautos", cr.compact.hitRDCautos);
    write_arr("hitCurFreeRoamSections", cr.compact.hitCurFreeRoamSections, true);

    ss << "  }\n}";
    return ss.str();
}

// tlog部分
struct BinaryReaderHelper {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;

    template<typename T> T read() {
        if (pos + sizeof(T) > size) throw std::runtime_error("二进制数据越界");
        T val;
        std::memcpy(&val, data + pos, sizeof(T));
        pos += sizeof(T);
        return val;
    }

    std::string readString() {
        uint32_t len = 0;
        int shift = 0;
        while (true) {
            uint8_t b = read<uint8_t>();
            len |= (b & 0x7F) << shift;
            if ((b & 0x80) == 0) break;
            shift += 7;
        }
        if (pos + len > size) throw std::runtime_error("字符串长度超出界限");
        std::string str((const char*)data + pos, len);
        pos += len;
        return str;
    }

    uint32_t readVarint() {
        uint32_t res = 0;
        int shift = 0;
        while (true) {
            uint8_t b = read<uint8_t>();
            res |= (b & 0x7F) << shift;
            if ((b & 0x80) == 0) break;
            shift += 7;
        }
        return res;
    }
};

LogSession parse_tlog_bytes(const std::vector<uint8_t>& bytes) {
    BinaryReaderHelper r{bytes.data(), bytes.size(), 0};
    if (bytes.size() < 5 || std::memcmp(r.data, "TSMZ", 4) != 0) throw std::runtime_error("无效的 TLOG 魔数");
    r.pos += 4;
    
    LogSession s;
    s.version = r.read<uint8_t>();
    s.timestamp = r.read<int64_t>();
    s.songName = r.readString();
    s.levelPath = r.readString();

    if (s.version >= 4) {
        s.bpm = r.read<double>();
        s.speed = r.read<double>();
        s.pitch = r.read<double>();
        s.isAngle = r.read<uint8_t>() != 0;

        int64_t prevTimingBits = 0;
        while (r.pos < r.size) {
            int64_t deltaBits = r.read<int64_t>();
            int64_t bits = deltaBits ^ prevTimingBits;
            prevTimingBits = bits;
            
            double val;
            std::memcpy(&val, &bits, sizeof(double));
            int margin = (int)r.readVarint();
            s.hits.push_back({val, margin});
        }
    } else {
        while (r.pos < r.size) {
            double val = r.read<double>();
            int margin = r.read<int32_t>();
            s.hits.push_back({val, margin});
        }
    }
    return s;
}

std::vector<uint8_t> build_tlog_bytes(const LogSession& s) {
    std::vector<uint8_t> buf;
    auto write_bytes = [&](const void* p, size_t sz) {
        const uint8_t* b = (const uint8_t*)p;
        buf.insert(buf.end(), b, b + sz);
    };
    auto write_str = [&](const std::string& str) {
        uint32_t len = (uint32_t)str.size();
        while (len >= 0x80) {
            uint8_t b = (uint8_t)(len | 0x80);
            buf.push_back(b);
            len >>= 7;
        }
        buf.push_back((uint8_t)len);
        write_bytes(str.data(), str.size());
    };

    write_bytes("TSMZ", 4);
    buf.push_back(s.version);
    write_bytes(&s.timestamp, 8);
    write_str(s.songName);
    write_str(s.levelPath);

    if (s.version >= 4) {
        write_bytes(&s.bpm, 8);
        write_bytes(&s.speed, 8);
        write_bytes(&s.pitch, 8);
        uint8_t angle = s.isAngle ? 1 : 0;
        buf.push_back(angle);

        int64_t prevBits = 0;
        for (const auto& hit : s.hits) {
            int64_t bits;
            std::memcpy(&bits, &hit.value, sizeof(double));
            int64_t delta = bits ^ prevBits;
            prevBits = bits;
            write_bytes(&delta, 8);

            uint32_t v = (uint32_t)hit.marginCode;
            while (v >= 0x80) {
                buf.push_back((uint8_t)(v | 0x80));
                v >>= 7;
            }
            buf.push_back((uint8_t)v);
        }
    } else {
        for (const auto& hit : s.hits) {
            write_bytes(&hit.value, 8);
            int32_t m = hit.marginCode;
            write_bytes(&m, 4);
        }
    }
    return buf;
}

// io
std::vector<uint8_t> read_file_raw(const std::string& path) {
    std::ifstream fs(path, std::ios::binary | std::ios::ate);
    if (!fs) throw std::runtime_error("无法打开文件: " + path);
    size_t size = fs.tellg();
    fs.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(size);
    fs.read((char*)buf.data(), size);
    return buf;
}

LogSession load_any_file(const std::string& path) {
    auto buf = read_file_raw(path);
    if (path.rfind(".gz") != std::string::npos || (buf.size() >= 2 && buf[0] == 0x1f && buf[1] == 0x8b)) {
        buf = decompress_gzip(buf);
    }
    if (buf.size() >= 4 && std::memcmp(buf.data(), "TSMZ", 4) == 0) {
        return parse_tlog_bytes(buf);
    } else {
        std::string json_str((char*)buf.data(), buf.size());
        return parse_json(json_str);
    }
}

void write_file_raw(const std::string& path, const void* data, size_t size) {
    std::ofstream fs(path, std::ios::binary);
    fs.write((const char*)data, size);
}


std::string clean_input_path(std::string path) {
    path.erase(path.begin(), std::find_if(path.begin(), path.end(), [](unsigned char ch) {
        return ch != '"' && !std::isspace(ch);
    }));
    path.erase(std::find_if(path.rbegin(), path.rend(), [](unsigned char ch) {
        return ch != '"' && !std::isspace(ch);
    }).base(), path.end());
    return path;
}




int main() {
    #ifdef _WIN32
    system("chcp 65001");
    #endif
    std::cout << "\n========== TimingLogger 数据转换工具 ===================================\n";
    std::cout << "1. JSON New/Old 格式转换                  2. JSON 转 TLOG / TLOG.GZ\n";
    std::cout << "3. TLOG / TLOG.GZ 转 JSON (New/Old)      4. TLOG.GZ 解压为 TLOG\n";
    std::cout << "5. JSON/TLOG 数据格式 V1/V2 互转          6. 查看日志元数据与偏差统计分析\n";
    std::cout << "7. CRPL2 转 TimingShow JSON (实验性)      8. CRPL2 解码 (实验性)\n";
    std::cout << "\n========================================================================\n";
    std::cout << "请选择: ";

    int choice;
    if (!(std::cin >> choice) || choice < 1 || choice > 8) {
        std::cerr << "无效的选择！\n";
        return 1;
    }

    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    std::string path;
    std::cout << "请输入目标文件路径: ";
    std::getline(std::cin, path);

    path = clean_input_path(path);

    try {
        if (choice == 1) { 
            LogSession session = load_any_file(path);
            std::cout << "请选择输出格式 (1: New 数组格式, 2: Old 对象格式): ";
            int subChoice;
            std::cin >> subChoice;
            std::string out = build_json(session, subChoice == 2);
            std::string outPath = path + ".converted.json";
            write_file_raw(outPath, out.data(), out.size());
            std::cout << "转换成功，已输出至: " << outPath << "\n";
        }
        else if (choice == 2) { 
            LogSession session = load_any_file(path);
            auto bytes = build_tlog_bytes(session);
            std::string outPath = path + ".tlog";
            write_file_raw(outPath, bytes.data(), bytes.size());
            std::cout << "转换成功，已输出至: " << outPath << "\n";
        }
        else if (choice == 3) { 
            LogSession session = load_any_file(path);
            std::cout << "请选择输出 JSON 格式 (1: New 数组格式, 2: Old 对象格式): ";
            int subChoice;
            std::cin >> subChoice;
            std::string out = build_json(session, subChoice == 2);
            std::string outPath = path + ".json";
            write_file_raw(outPath, out.data(), out.size());
            std::cout << "转换成功，已输出至: " << outPath << "\n";
        }
        else if (choice == 4) {
            auto raw = read_file_raw(path);
            auto decompressed = decompress_gzip(raw);
            std::string outPath = path;
            if (outPath.rfind(".gz") != std::string::npos) {
                outPath = outPath.substr(0, outPath.rfind(".gz"));
            } else {
                outPath += ".decompressed.tlog";
            }
            write_file_raw(outPath, decompressed.data(), decompressed.size());
            std::cout << "解压成功，已输出至: " << outPath << "\n";
        }
        else if (choice == 5) { 
            LogSession session = load_any_file(path);
            std::cout << "当前文件检测到为: V" << (session.version >= 4 ? 2 : 1) << "\n";
            std::cout << "请选择目标转换版本 (1/2): ";
            int verChoice;
            std::cin >> verChoice;

            if (verChoice == 2 && session.version < 4) {
                std::cout << "\n补全缺失的元数据信息\n";
                
                std::cout << "请输入 BPM: ";
                while (!(std::cin >> session.bpm)) {
                    std::cin.clear(); std::cin.ignore(10000, '\n');
                    std::cout << "输入无效: ";
                }

                std::cout << "请输入 Speed: ";
                while (!(std::cin >> session.speed)) {
                    std::cin.clear(); std::cin.ignore(10000, '\n');
                    std::cout << "输入无效: ";
                }

                std::cout << "请输入 Pitch: ";
                while (!(std::cin >> session.pitch)) {
                    std::cin.clear(); std::cin.ignore(10000, '\n');
                    std::cout << "输入无效: ";
                }

                std::cout << "数值记录类型为Angle还是Timing? (1:Angle/0:Timing): ";
                int angleChoice;
                while (!(std::cin >> angleChoice) || (angleChoice != 0 && angleChoice != 1)) {
                    std::cin.clear(); std::cin.ignore(10000, '\n');
                    std::cout << "输入无效: ";
                }
                session.isAngle = (angleChoice == 1);
            }

            session.version = (verChoice == 2) ? 4 : 1;

            std::string outPath;
            if (path.rfind(".json") != std::string::npos) {
                std::string out = build_json(session, false);
                outPath = path + ".v" + std::to_string(verChoice) + ".json";
                write_file_raw(outPath, out.data(), out.size());
            } else {
                auto bytes = build_tlog_bytes(session);
                outPath = path + ".v" + std::to_string(verChoice) + ".tlog";
                write_file_raw(outPath, bytes.data(), bytes.size());
            }
            std::cout << "\n版本转换完成，已写入至: " << outPath << "\n";
        }
        else if (choice == 6) { 
            LogSession session = load_any_file(path);

            std::time_t ts = static_cast<std::time_t>(session.timestamp);
            std::tm tm_buf;
            #ifdef _WIN32
            gmtime_s(&tm_buf, &ts);
            #else
            gmtime_r(&ts, &tm_buf);
            #endif
            char time_str[64];
            std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S UTC", &tm_buf);

            double sum = 0.0;
            double max_val = session.hits.empty() ? 0.0 : session.hits[0].value;
            double min_val = session.hits.empty() ? 0.0 : session.hits[0].value;

            for (const auto& hit : session.hits) {
                sum += hit.value;
                if (hit.value > max_val) max_val = hit.value;
                if (hit.value < min_val) min_val = hit.value;
            }
            double avg_offset = session.hits.empty() ? 0.0 : (sum / session.hits.size());

            std::string offsets_fmt = "N/A (Binary)";
            if (path.rfind(".json") != std::string::npos) {
                auto raw_buf = read_file_raw(path);
                std::string json_content((char*)raw_buf.data(), raw_buf.size());
                if (json_content.find("\"offsets\": {") != std::string::npos || 
                    json_content.find("\"offsets\":{") != std::string::npos) {
                    offsets_fmt = "Old (Object)";
                } else if (json_content.find("\"offsets\": [") != std::string::npos || 
                           json_content.find("\"offsets\":[") != std::string::npos) {
                    offsets_fmt = "New (Array)";
                }
            }

            std::cout << "\n================== 基本信息 ==================\n";
            std::cout << "文件路径          : " << path << "\n";
            std::cout << "曲目名称          : " << session.songName << "\n";
            std::cout << "关卡路径          : " << session.levelPath << "\n";
            std::cout << "数据格式版本      : V" << (session.version >= 4 ? "2 (4)" : "1 (1)") << "\n";
            std::cout << "Offsets 结构类型  : " << offsets_fmt << "\n";
            std::cout << "记录时间          : " << time_str << " (" << session.timestamp << ")\n";
            
            if (session.version >= 4) {
                std::cout << "BPM               : " << session.bpm << "\n";
                std::cout << "Speed             : " << session.speed << "\n";
                std::cout << "Pitch             : " << session.pitch << "\n";
                std::cout << "数值模式          : " << (session.isAngle ? "Angle" : "Timing") << "\n";
            } 

            std::cout << "\n================== 数据分析 ==================\n";
            std::cout << "总击打次数 (Hits) : " << session.hits.size() << "\n";
            if (!session.hits.empty()) {
                std::cout << "平均偏差    : " << avg_offset << (session.isAngle ? " °" : " ms") << "\n";
                std::cout << "最大偏差    : " << max_val << (session.isAngle ? " °" : " ms") << "\n";
                std::cout << "最小偏差    : " << min_val << (session.isAngle ? " °" : " ms") << "\n";
                std::cout << "极差        : " << (max_val - min_val) << (session.isAngle ? " °" : " ms") << "\n";
            }
            std::cout << "==============================================\n";
        }
        else if (choice == 7) { 
            Crpl2Data cr = decrypt_crpl2_file(path);

            LogSession s;
            s.version = 4;
            s.isAngle = true; 

            for (size_t k = 0; k < cr.compact.s_keys.size(); ++k) {
                if (cr.compact.s_keys[k] == "songName" || cr.compact.s_keys[k] == "path") s.songName = cr.compact.s_vals[k];
                if (cr.compact.s_keys[k] == "levelPath" || cr.compact.s_keys[k] == "author") s.levelPath = cr.compact.s_vals[k];
            }
            for (size_t k = 0; k < cr.compact.d_keys.size(); ++k) {
                if (cr.compact.d_keys[k] == "bpm") s.bpm = cr.compact.d_vals[k];
                if (cr.compact.d_keys[k] == "speed" || cr.compact.d_keys[k] == "bpmTile") s.speed = cr.compact.d_vals[k];
                if (cr.compact.d_keys[k] == "pitch") s.pitch = cr.compact.d_vals[k];
            }
            s.timestamp = std::time(nullptr);

            for (double angle : cr.compact.hitCurrAngles) {
                s.hits.push_back({angle, 3});
            }

            std::cout << "请选择 JSON 数组格式 (1: New 数组格式 [angle, 3], 2: Old 对象格式): ";
            int subChoice;
            std::cin >> subChoice;

            std::string json_str = build_json(s, subChoice == 2);
            std::string outPath = path + ".timingshow.json";
            write_file_raw(outPath, json_str.data(), json_str.size());
            std::cout << "\n转换完成!\n已保存至: " << outPath << "\n";
        }
        else if (choice == 8) { 
            Crpl2Data cr = decrypt_crpl2_file(path);
            std::string raw_json = build_crpl2_json(cr);
            std::string outPath = path + ".raw.json";
            write_file_raw(outPath, raw_json.data(), raw_json.size());
            std::cout << "\n解码完成!\n已保存至: " << outPath << "\n";
        }

        #ifdef _WIN32
        system("pause");
        #endif
    } 
    catch (const std::exception& e) {
        std::cerr << "\n处理失败: " << e.what() << "\n";
        return 1;
    }

    return 0;
}