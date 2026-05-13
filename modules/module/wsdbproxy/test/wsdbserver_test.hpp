#ifndef WSDBSERVER_TEST_HPP
#define WSDBSERVER_TEST_HPP

#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>
#include "wsdbproxy.hpp"
#include "wsframe.hpp"

// Helper: write one WebSocket binary frame carrying BSON bytes to fd.
inline bool send_ws_binary(int fd, const std::vector<uint8_t>& bson)
{
    auto frame = wsframe::encode(bson, 0x02, true);  // masked (client→server)
    ssize_t sent = ::send(fd, frame.data(), frame.size(), 0);
    return sent == static_cast<ssize_t>(frame.size());
}

// Helper: read one WebSocket frame from fd, return payload bytes.
// Returns empty on error.
inline std::vector<uint8_t> recv_ws_binary(int fd)
{
    std::vector<uint8_t> buf(65536);
    ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
    if (n <= 0) return {};
    buf.resize(n);
    auto frame = wsframe::decode(buf);
    if (!frame || frame->opcode != 0x02) return {};
    return frame->payload;
}

#endif // WSDBSERVER_TEST_HPP
