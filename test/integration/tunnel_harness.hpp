#ifndef TUNNEL_HARNESS_HPP
#define TUNNEL_HARNESS_HPP

/// @file tunnel_harness.hpp
///
/// In-memory wiring of every Layer 0–2 pure-logic component into a
/// single end-to-end harness. Used by `tunnel_e2e_test.cc` (Layer 3) to
/// exercise the full frame plumbing without any real sockets, ACE, or
/// Asterisk.
///
/// Topology:
///
///        Cloud (Heroku side)                  Agent (on-prem side)
///   ┌──────────────────────────┐         ┌─────────────────────────────┐
///   │ SipBridge ◀──tunnel──▶ CloudTunnelEndpoint │
///   │   │                                 │   ▲                       │
///   │   ▼                                 │   │                       │
///   │ BrowserSink (test fake)             │   │   ┌───────────────┐   │
///   │                                     │   ▼   │FakeAsterisk   │   │
///   │                                     │ SipFrameDemux ◀───────┤   │
///   │                                     │   ▲                   │   │
///   │                                     │   │ tunnel            │   │
///   │                                     │ CloudConnector        │   │
///   │                                     │   ▲                   │   │
///   │                                     │   │ ITransport (in-mem)   │
///   └──────────────────────────────┘     └────┴───────────────────────┘
///
/// The two in-memory transports are paired — bytes written by the
/// agent's `ITransport::send` appear at the cloud's
/// `CloudTunnelEndpoint::on_bytes_received`, and vice versa. Closing
/// either side fires both sides' disconnect callbacks.

#include "cloud_connector.hpp"
#include "cloud_tunnel_endpoint.hpp"
#include "sip_bridge.hpp"
#include "sip_frame_demux.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tunnel_e2e {

class TunnelHarness;

// ─────────────────────────────────────────────────────────────────────────────
// Paired in-memory transports. Each side holds a pointer to the harness so
// it can route bytes to the peer's `on_bytes_received` and propagate close
// notifications. State lives on the harness so it survives the moment when
// either side's `unique_ptr<ITransport>` is destroyed.
// ─────────────────────────────────────────────────────────────────────────────

class InMemoryAgentSideTransport : public ITransport {
public:
    explicit InMemoryAgentSideTransport(TunnelHarness *h) : m_h(h) {}
    bool send(const std::string &bytes) override;
    void close() override;

private:
    TunnelHarness *m_h;
    bool m_closed = false;
};

class InMemoryCloudSideTransport : public IAgentTransport {
public:
    explicit InMemoryCloudSideTransport(TunnelHarness *h) : m_h(h) {}
    bool send(const std::string &bytes) override;
    void close() override;

private:
    TunnelHarness *m_h;
    bool m_closed = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// Factory used by CloudConnector. On the first `create_connected` call, builds
// a fresh in-memory transport pair AND simultaneously hands the cloud-side
// transport to the harness's CloudTunnelEndpoint — that's the integration
// counterpart of WebConnection's /agent WS upgrade hand-off.
// ─────────────────────────────────────────────────────────────────────────────

class HarnessTransportFactory : public ITransportFactory {
public:
    explicit HarnessTransportFactory(TunnelHarness *h) : m_h(h) {}
    std::unique_ptr<ITransport>
    create_connected(const std::string &, std::uint16_t, const std::string &,
                     const std::string &, const std::string &) override;
    void refuse_next() { refusal = true; }

    int attempts = 0;
    bool refusal = false;

private:
    TunnelHarness *m_h;
};

class HarnessClock : public IClock {
public:
    std::int64_t t = 0;
    std::int64_t now_unix() const override { return t; }
    void advance(std::int64_t sec) { t += sec; }
};

// ─────────────────────────────────────────────────────────────────────────────
// Per-stream Asterisk fake. State outlives the `unique_ptr<IAsteriskStream>`.
// Same fake-side-channel pattern as SipFrameDemux's own tests.
// ─────────────────────────────────────────────────────────────────────────────

struct AsteriskState {
    std::vector<std::string> received;
    bool closed = false;
    std::string close_reason;
};

class FakeAsterisk : public IAsteriskStream {
public:
    explicit FakeAsterisk(AsteriskState *s) : m_s(s) {}
    bool send_bytes(const std::string &b) override { m_s->received.push_back(b); return true; }
    void close(const std::string &r) override { m_s->closed = true; m_s->close_reason = r; }

private:
    AsteriskState *m_s;
};

class HarnessAsteriskFactory : public IAsteriskFactory {
public:
    std::unordered_map<std::uint32_t, std::unique_ptr<AsteriskState>> by_sid;
    std::vector<std::pair<std::uint32_t, std::string>> opens;

    std::unique_ptr<IAsteriskStream> open(std::uint32_t sid,
                                          const std::string &meta) override {
        opens.push_back({sid, meta});
        auto state = std::make_unique<AsteriskState>();
        AsteriskState *raw = state.get();
        by_sid.emplace(sid, std::move(state));
        return std::make_unique<FakeAsterisk>(raw);
    }

    AsteriskState *state_for(std::uint32_t sid) {
        auto it = by_sid.find(sid);
        return (it == by_sid.end()) ? nullptr : it->second.get();
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// BrowserSink that records bytes / close on the cloud side.
// ─────────────────────────────────────────────────────────────────────────────

class FakeBrowser : public BrowserSink {
public:
    std::vector<std::string> got;
    bool closed = false;
    std::string close_reason;
    void send_bytes(const std::string &b) override { got.push_back(b); }
    void close(const std::string &r) override { closed = true; close_reason = r; }
};

// ─────────────────────────────────────────────────────────────────────────────
// The harness — owns every cloud-side and agent-side state machine, plus the
// paired transports.
// ─────────────────────────────────────────────────────────────────────────────

class TunnelHarness {
public:
    // Cloud side
    CloudTunnelEndpoint cloud_endpoint;
    SipBridge           cloud_bridge{&cloud_endpoint};
    // Push / CDR landing pads on the cloud side (Layer 3 wires PushSender +
    // Mongo writer here in production).
    std::vector<std::string> push_payloads;
    std::vector<std::string> cdr_payloads;

    // Agent side
    HarnessClock          clock;
    HarnessAsteriskFactory ast_factory;
    HarnessTransportFactory tport_factory{this};
    CloudConnector        agent_connector{default_config(), tport_factory, clock};
    SipFrameDemux         agent_demux{&agent_connector, ast_factory};

    // Live raw pointers to the paired transports while they're connected.
    // Either side's close() sets these to nullptr.
    InMemoryAgentSideTransport *agent_xport_live = nullptr;
    InMemoryCloudSideTransport *cloud_xport_live = nullptr;

    TunnelHarness() {
        cloud_endpoint.attach_bridge(&cloud_bridge);
        agent_connector.attach_demux(&agent_demux);
        cloud_bridge.set_push_notify_handler(
            [this](const std::string &p) { push_payloads.push_back(p); });
        cloud_bridge.set_cdr_push_handler(
            [this](const std::string &p) { cdr_payloads.push_back(p); });
    }

    /// Drive the connector once. After the first call the agent + cloud are
    /// fully paired and frames flow end-to-end.
    void connect() { agent_connector.tick(); }

    /// Simulate the agent's CloudConnector seeing its transport drop.
    /// Triggers both sides' cleanup paths.
    void agent_side_drop() {
        // We close from the agent end; the close() impl signals the cloud's
        // on_agent_disconnected and the connector's on_transport_lost.
        if (agent_xport_live) agent_xport_live->close();
    }

    /// Simulate the cloud receiving an EOF from the agent — same logical
    /// outcome, different initiator.
    void cloud_side_drop() {
        if (cloud_xport_live) cloud_xport_live->close();
    }

    /// Called by InMemoryAgentSideTransport::send — deliver bytes upstream
    /// to the cloud endpoint.
    void deliver_to_cloud(const std::string &bytes) {
        cloud_endpoint.on_bytes_received(bytes);
    }

    /// Called by InMemoryCloudSideTransport::send — deliver bytes downstream
    /// to the agent connector.
    void deliver_to_agent(const std::string &bytes) {
        agent_connector.on_bytes_received(bytes);
    }

    /// Called when EITHER transport is closed — tear down both sides.
    void on_either_close(bool from_agent_side) {
        // First clear the live pointers so re-entrant close() is a no-op.
        agent_xport_live = nullptr;
        cloud_xport_live = nullptr;

        if (from_agent_side) {
            // Tell the connector the transport is gone (also tells the demux).
            agent_connector.on_transport_lost();
            cloud_endpoint.on_agent_disconnected();
        } else {
            cloud_endpoint.on_agent_disconnected();
            agent_connector.on_transport_lost();
        }
    }

private:
    static CloudConnector::Config default_config() {
        CloudConnector::Config c;
        c.host                  = "harness";
        c.port                  = 0;
        c.client_cert_path      = "/dev/null";
        c.client_key_path       = "/dev/null";
        c.server_ca_path        = "/dev/null";
        c.initial_backoff_sec   = 1;
        c.max_backoff_sec       = 1;
        return c;
    }
};

// ── Definitions deferred until TunnelHarness is complete ────────────────────

inline bool InMemoryAgentSideTransport::send(const std::string &bytes) {
    if (m_closed || !m_h) return false;
    m_h->deliver_to_cloud(bytes);
    return true;
}
inline void InMemoryAgentSideTransport::close() {
    if (m_closed) return;
    m_closed = true;
    if (m_h) m_h->on_either_close(/*from_agent_side=*/true);
}

inline bool InMemoryCloudSideTransport::send(const std::string &bytes) {
    if (m_closed || !m_h) return false;
    m_h->deliver_to_agent(bytes);
    return true;
}
inline void InMemoryCloudSideTransport::close() {
    if (m_closed) return;
    m_closed = true;
    if (m_h) m_h->on_either_close(/*from_agent_side=*/false);
}

inline std::unique_ptr<ITransport>
HarnessTransportFactory::create_connected(const std::string &,
                                           std::uint16_t,
                                           const std::string &,
                                           const std::string &,
                                           const std::string &) {
    ++attempts;
    if (refusal) {
        refusal = false;
        return nullptr;
    }
    // Build the pair.
    auto agent_side = std::make_unique<InMemoryAgentSideTransport>(m_h);
    auto cloud_side = std::make_unique<InMemoryCloudSideTransport>(m_h);
    m_h->agent_xport_live = agent_side.get();
    m_h->cloud_xport_live = cloud_side.get();
    // Cloud side takes the cloud-bound transport.
    m_h->cloud_endpoint.on_agent_connected(std::move(cloud_side));
    return agent_side;
}

} // namespace tunnel_e2e

#endif // TUNNEL_HARNESS_HPP
