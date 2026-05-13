#ifndef WSTRANSPORT_HPP
#define WSTRANSPORT_HPP

#include "innertls.hpp"

#include <cstdint>
#include <functional>
#include <vector>

/**
 * @brief Adapts WebSocket frame send/recv functions to the ITransport interface.
 *
 * Used by both wsdbagent and WsDbServer to layer inner TLS over WebSocket
 * binary frames.  Ping/pong handling is transparent — recv() loops until a
 * binary frame arrives (auto-replying to pings), and close frames cause
 * recv() to return false.
 */
class WebSocketTransport : public ITransport {
public:
  using SendFn = std::function<bool(const std::vector<std::uint8_t>&, std::uint8_t)>;
  using RecvFn = std::function<bool(std::uint8_t&, std::vector<std::uint8_t>&)>;

  WebSocketTransport(SendFn send_frame, RecvFn recv_frame)
    : m_sendFrame(std::move(send_frame))
    , m_recvFrame(std::move(recv_frame))
  {}

  bool send(const std::vector<std::uint8_t>& data) override {
    return m_sendFrame(data, 0x02);
  }

  bool recv(std::vector<std::uint8_t>& data) override {
    std::uint8_t opcode = 0;
    std::vector<std::uint8_t> payload;
    if (!m_recvFrame(opcode, payload)) return false;

    switch (opcode) {
    case 0x02:  // binary — TLS record
      data = std::move(payload);
      return true;
    case 0x09:  // ping → pong
      m_sendFrame(payload, 0x0A);
      data.clear();
      return true;
    case 0x0A:  // pong — ignore
      data.clear();
      return true;
    case 0x08:  // close
      return false;
    default:
      data.clear();
      return true;
    }
  }

private:
  SendFn m_sendFrame;
  RecvFn m_recvFrame;
};

#endif // WSTRANSPORT_HPP
