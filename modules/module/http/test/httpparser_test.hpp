#ifndef HTTPPARSER_TEST_HPP
#define HTTPPARSER_TEST_HPP

#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <zlib.h>
#include "http_parser.hpp"

/// Gzip-compress a string for use in Content-Encoding tests.
static inline std::string gzip_compress(const std::string& in)
{
    z_stream zs{};
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                     15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return {};

    zs.next_in  = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    zs.avail_in = static_cast<uInt>(in.size());

    std::string out;
    int ret;
    do {
        char buf[32768];
        zs.next_out  = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        ret = deflate(&zs, Z_FINISH);
        out.append(buf, sizeof(buf) - zs.avail_out);
    } while (ret == Z_OK);

    deflateEnd(&zs);
    return (ret == Z_STREAM_END) ? out : std::string{};
}

/// Encode a payload as a single HTTP/1.1 chunked block.
static inline std::string chunked_encode(const std::string& data)
{
    std::ostringstream ss;
    ss << std::hex << data.size() << "\r\n" << data << "\r\n0\r\n\r\n";
    return ss.str();
}

#endif // HTTPPARSER_TEST_HPP
