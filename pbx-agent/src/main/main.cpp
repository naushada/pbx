// pbx-agent — production entry point.
//
// Wires every Layer 0–3 component into a single ACE reactor and runs the
// event loop until SIGTERM/SIGINT. No new logic lives here — this is
// pure glue.
//
// Topology (matches the architecture diagram in README.md):
//
//   MongodbClient                    (local Mongo for CDR + subscriber data)
//   AriClient ◄── AriWsClient        (admission counter + CDR writer)
//        │  └─ CallRouter            (dialed ext → forked-ring originate/bridge)
//        │
//        ▼ PUSH_NOTIFY / CDR_PUSH via …
//   CloudConnector ◄── AceSslTransportFactory   (outbound mTLS to Heroku)
//        │
//        └─ SipFrameDemux ◄── AsteriskWsFactory (per-stream WS to local
//                                                  Asterisk's chan_pjsip)

#include "ace_ssl_transport.hpp"
#include "ari_client.hpp"
#include "ari_rest_client.hpp"
#include "ari_ws_client.hpp"
#include "asterisk_ws_factory.hpp"
#include "call_router.hpp"
#include "cloud_connector.hpp"
#include "json.hpp"
#include "mongodbc.hpp"
#include "pjsip_provisioner.hpp"
#include "sip_frame_demux.hpp"
#include "subscriber_watcher.hpp"

#include "ace/Get_Opt.h"
#include "ace/Log_Msg.h"
#include "ace/OS_NS_unistd.h"
#include "ace/Reactor.h"
#include "ace/Signal.h"
#include "ace/Time_Value.h"

#include <chrono>
#include <csignal>
#include <ctime>
#include <memory>
#include <string>
#include <thread>

// ─────────────────────────────────────────────────────────────────────────────
// AceSystemClock — production IClock for CloudConnector.
// ─────────────────────────────────────────────────────────────────────────────

class AceSystemClock : public IClock {
public:
  std::int64_t now_unix() const override {
    return static_cast<std::int64_t>(std::time(nullptr));
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// ReconnectSupervisor — periodic tick on the reactor thread.
//
// Schedules itself to fire every second:
//   - `CloudConnector::tick()` — drives reconnect-backoff and outbound flush.
//   - `AriWsClient::connect_and_handshake()` if it's currently disconnected.
//
// This is the only piece of "production logic" in this file. Everything
// else just instantiates and wires.
// ─────────────────────────────────────────────────────────────────────────────

class ReconnectSupervisor : public ACE_Event_Handler {
public:
  ReconnectSupervisor(CloudConnector &cc, AriWsClient &ari, ACE_Reactor *reactor)
      : m_cc(cc), m_ari(ari) {
    this->reactor(reactor);
  }

  int schedule_first_tick() {
    return reactor()->schedule_timer(this, nullptr,
                                      ACE_Time_Value(1, 0),  // first fire
                                      ACE_Time_Value(1, 0)); // repeat every 1s
  }

  int handle_timeout(const ACE_Time_Value &, const void *) override {
    m_cc.tick();
    if (!m_ari.connected()) {
      if (m_ari.connect_and_handshake()) {
        if (m_ari.register_with_reactor(reactor()) == -1) {
          ACE_ERROR((LM_ERROR,
                     ACE_TEXT("%D [pbx-agent] AriWsClient register_handler "
                              "failed\n")));
        }
      }
    }
    return 0;
  }

private:
  CloudConnector &m_cc;
  AriWsClient    &m_ari;
};

// ─────────────────────────────────────────────────────────────────────────────
// SubscriberWatcherTimer — drives `SubscriberWatcher::tick()` from the
// reactor at ~200 ms. Each tick drains one Mongo change-stream event,
// keeping the reactor responsive even if a burst of subscriber edits
// arrives. Cadence is independent from `ReconnectSupervisor`'s 1 s.
// ─────────────────────────────────────────────────────────────────────────────

class SubscriberWatcherTimer : public ACE_Event_Handler {
public:
  SubscriberWatcherTimer(SubscriberWatcher &watcher, ACE_Reactor *reactor)
      : m_watcher(watcher) {
    this->reactor(reactor);
  }

  int schedule_first_tick() {
    // 200 ms delay before first fire, then every 200 ms.
    return reactor()->schedule_timer(this, nullptr,
                                      ACE_Time_Value(0, 200000),
                                      ACE_Time_Value(0, 200000));
  }

  int handle_timeout(const ACE_Time_Value &, const void *) override {
    m_watcher.tick();
    return 0;
  }

private:
  SubscriberWatcher &m_watcher;
};

// ─────────────────────────────────────────────────────────────────────────────
// Signal handler — terminates the reactor loop cleanly.
// ─────────────────────────────────────────────────────────────────────────────

class ShutdownHandler : public ACE_Event_Handler {
public:
  int handle_signal(int signum, siginfo_t *, ucontext_t *) override {
    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("%D [pbx-agent] signal %d received — shutting down\n"),
               signum));
    ACE_Reactor::instance()->end_reactor_event_loop();
    return 0;
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// CLI parsing.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

void print_usage(const char *prog) {
  ACE_ERROR((LM_ERROR,
             ACE_TEXT("Usage: %s [OPTIONS]\n\n"
                      "  --cloud-host        <host>  Heroku hostname (e.g. pabx-5fbf3550f938.herokuapp.com)\n"
                      "  --cloud-port        <n>     Heroku port           (default: 443)\n"
                      "  --tls-cert          <path>  Client cert (PEM) for mTLS to Heroku\n"
                      "  --tls-key           <path>  Client private key (PEM)\n"
                      "  --tls-ca            <path>  CA cert (PEM) to verify Heroku's cert\n"
                      "  --mongo-uri         <uri>   MongoDB URI (e.g. mongodb://localhost:27017/pabx)\n"
                      "  --society-id        <id>    Society id this agent serves\n"
                      "  --asterisk-host     <host>  Asterisk hostname     (default: 127.0.0.1)\n"
                      "  --asterisk-port     <n>     Asterisk port         (default: 8088)\n"
                      "  --ari-app           <name>  Stasis app name       (default: pbx)\n"
                      "  --ari-user          <user>  ARI Basic-auth username (default: asterisk)\n"
                      "  --ari-pass          <pass>  ARI Basic-auth password (default: asterisk)\n"
                      "  --sip-realm         <r>     SIP auth realm           (default: <society-id>.pbx.local)\n"
                      "  --inner-tls-cert    <path>  Inner-TLS client cert  (PEM, REQUIRED for cloud /agent)\n"
                      "  --inner-tls-key     <path>  Inner-TLS client key   (PEM, REQUIRED)\n"
                      "  --inner-tls-ca      <path>  Inner-TLS CA for cloud cert verification (PEM, REQUIRED)\n"
                      "  --inner-tls-hostname <name> Expected cloud cert CN/SAN (optional but recommended)\n"
                      "  --help                      Show this help\n"),
             prog));
}

} // namespace

int main(int argc, char *argv[]) {
  ACE_LOG_MSG->open(argv[0], ACE_LOG_MSG->STDERR | ACE_LOG_MSG->SYSLOG);
  ACE_LOG_MSG->priority_mask(LM_CRITICAL | LM_ERROR | LM_WARNING |
                              LM_DEBUG | LM_INFO,
                              ACE_Log_Msg::PROCESS);

  std::string cloud_host;
  int         cloud_port = 443;
  std::string tls_cert, tls_key, tls_ca;
  std::string mongo_uri;
  std::string society_id;
  std::string ast_host = "127.0.0.1";
  int         ast_port = 8088;
  std::string ari_app  = "pbx";
  std::string ari_user = "asterisk";
  std::string ari_pass = "asterisk";
  std::string sip_realm;  // defaults to <society-id>.pbx.local after parsing
  std::string inner_tls_cert, inner_tls_key, inner_tls_ca, inner_tls_hostname;

  ACE_Get_Opt args(argc, argv, ACE_TEXT("H:P:E:K:A:U:S:a:p:n:u:w:r:i:j:l:m:h"), 1);
  args.long_option(ACE_TEXT("cloud-host"),     'H', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("cloud-port"),     'P', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("tls-cert"),       'E', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("tls-key"),        'K', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("tls-ca"),         'A', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("mongo-uri"),      'U', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("society-id"),     'S', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("asterisk-host"),  'a', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("asterisk-port"),  'p', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("ari-app"),        'n', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("ari-user"),       'u', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("ari-pass"),       'w', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("sip-realm"),      'r', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("inner-tls-cert"), 'i', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("inner-tls-key"),  'j', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("inner-tls-ca"),   'l', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("inner-tls-hostname"), 'm', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("help"),           'h', ACE_Get_Opt::NO_ARG);

  for (int c; (c = args()) != EOF;) {
    switch (c) {
    case 'H': cloud_host = args.opt_arg(); break;
    case 'P': cloud_port = std::stoi(args.opt_arg()); break;
    case 'E': tls_cert   = args.opt_arg(); break;
    case 'K': tls_key    = args.opt_arg(); break;
    case 'A': tls_ca     = args.opt_arg(); break;
    case 'U': mongo_uri  = args.opt_arg(); break;
    case 'S': society_id = args.opt_arg(); break;
    case 'a': ast_host   = args.opt_arg(); break;
    case 'p': ast_port   = std::stoi(args.opt_arg()); break;
    case 'n': ari_app    = args.opt_arg(); break;
    case 'u': ari_user   = args.opt_arg(); break;
    case 'w': ari_pass   = args.opt_arg(); break;
    case 'r': sip_realm  = args.opt_arg(); break;
    case 'i': inner_tls_cert     = args.opt_arg(); break;
    case 'j': inner_tls_key      = args.opt_arg(); break;
    case 'l': inner_tls_ca       = args.opt_arg(); break;
    case 'm': inner_tls_hostname = args.opt_arg(); break;
    case 'h': print_usage(argv[0]); return 0;
    case '?': print_usage(argv[0]); return -1;
    default:  break;
    }
  }

  if (cloud_host.empty() || mongo_uri.empty() || society_id.empty()) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("--cloud-host, --mongo-uri and --society-id are "
                        "required\n")));
    print_usage(argv[0]);
    return -1;
  }

  // Default the SIP realm to the cloud-side convention (see
  // microservice_pbx.cpp handle_society_POST: `code + ".pbx.local"`). If a
  // society's code differs from its id the operator should pass the
  // explicit realm via --sip-realm.
  if (sip_realm.empty()) sip_realm = society_id + ".pbx.local";

  ACE_DEBUG((LM_INFO,
             ACE_TEXT("%D [pbx-agent] starting: cloud=%s:%d society=%s "
                      "asterisk=%s:%d\n"),
             cloud_host.c_str(), cloud_port, society_id.c_str(),
             ast_host.c_str(), ast_port));

  // ── Reactor + signal handling ──────────────────────────────────────────
  ACE_Reactor *reactor = ACE_Reactor::instance();
  ShutdownHandler shutdown_handler;
  reactor->register_handler(SIGINT,  &shutdown_handler);
  reactor->register_handler(SIGTERM, &shutdown_handler);

  // ── MongoDB ────────────────────────────────────────────────────────────
  auto db = std::make_unique<MongodbClient>(mongo_uri);

  // ── AriClient + AriRestClient + AriWsClient ────────────────────────────
  AriRestClient::Config ari_rest_cfg;
  ari_rest_cfg.host     = ast_host;
  ari_rest_cfg.port     = static_cast<std::uint16_t>(ast_port);
  ari_rest_cfg.username = ari_user;
  ari_rest_cfg.password = ari_pass;
  AriRestClient ari_rest(ari_rest_cfg);

  // CallRouter resolves a dialed extension to its SIP targets and drives
  // the forked-ring originate/bridge state machine; AriClient feeds it
  // the ARI events.
  CallRouter call_router(society_id, *db, ari_rest, ari_app);

  AriClient::Config ari_cfg;
  ari_cfg.society_id = society_id;
  ari_cfg.app_name   = ari_app;
  AriClient ari_client(ari_cfg, ari_rest, *db, call_router);
  ari_client.start();  // POSTs the subscription to Asterisk

  // ── PjsipProvisioner + SubscriberWatcher ───────────────────────────────
  //
  // Materialise every active `subscribers` row as auth/aor/endpoint sorcery
  // objects in Asterisk via ARI dynamic-config, then tail the Mongo change
  // stream to keep them in sync as admins create/disable/delete subscribers
  // upstream. Bootstrap runs synchronously here — pre-existing rows are
  // pushed before the reactor starts. ARI errors during bootstrap are
  // logged-and-dropped; the next change-stream event re-applies. Change
  // streams require Mongo to be a (single-node) replica set — see
  // docker-compose.agent.yml's `pbx-mongo` service. On a standalone mongod
  // bootstrap still runs but `tick()` becomes a silent no-op.
  PjsipProvisioner pjsip(ari_rest, sip_realm);
  SubscriberWatcher watcher(*db, society_id, pjsip);
  watcher.bootstrap();

  AriWsClient::Config ari_ws_cfg;
  ari_ws_cfg.host     = ast_host;
  ari_ws_cfg.port     = static_cast<std::uint16_t>(ast_port);
  ari_ws_cfg.app_name = ari_app;
  ari_ws_cfg.username = ari_user;
  ari_ws_cfg.password = ari_pass;
  AriWsClient ari_ws(ari_ws_cfg,
                      [&](const std::string &event) {
                        ari_client.on_event(event);
                      });

  // ── AsteriskWsFactory needs the demux + reactor at construction.
  // Both are known by the time we reach this point in main(). We
  // construct it after CloudConnector + SipFrameDemux below so the
  // factory has the demux reference, but assemble the variables here
  // so the comment + ordering reads top-to-bottom.

  // ── CloudConnector + AceSslTransportFactory ────────────────────────────
  CloudConnector::Config cc_cfg;
  cc_cfg.host             = cloud_host;
  cc_cfg.port             = static_cast<std::uint16_t>(cloud_port);
  cc_cfg.client_cert_path = tls_cert;
  cc_cfg.client_key_path  = tls_key;
  cc_cfg.server_ca_path   = tls_ca;
  AceSystemClock clock;

  // Forward declarations to capture in the factory's callbacks.
  CloudConnector *cc_ptr  = nullptr;
  SipFrameDemux  *demux_ptr = nullptr;

  // Inner-TLS over the WS — the real mTLS trust boundary (Heroku
  // terminates the outer TLS at its router, so `--tls-cert/--tls-key`
  // are never round-tripped against a verifiable peer). Mirror of the
  // /ws/db pattern. Empty cert path = inner TLS disabled (test wiring
  // only — production cloud requires it on every /agent dial).
  AceSslTransport::InnerTlsConfig agent_inner_tls;
  agent_inner_tls.cert_path = inner_tls_cert;
  agent_inner_tls.key_path  = inner_tls_key;
  agent_inner_tls.ca_path   = inner_tls_ca;
  agent_inner_tls.hostname  = inner_tls_hostname;

  AceSslTransportFactory transport_factory(
      reactor,
      [&](const std::string &bytes) {
        if (cc_ptr) cc_ptr->on_bytes_received(bytes);
      },
      [&]() {
        if (cc_ptr) cc_ptr->on_transport_lost();
      },
      agent_inner_tls);

  CloudConnector connector(cc_cfg, transport_factory, clock);
  cc_ptr = &connector;

  // We need an IAsteriskFactory ref for SipFrameDemux's ctor; build a
  // shim that defers to the real AsteriskWsFactory once it exists. This
  // breaks the construction-order cycle (demux needs factory; factory
  // needs demux ref).
  class DeferredAsteriskFactory : public IAsteriskFactory {
  public:
    IAsteriskFactory *real = nullptr;
    std::unique_ptr<IAsteriskStream> open(std::uint32_t sid,
                                           const std::string &meta) override {
      return real ? real->open(sid, meta) : nullptr;
    }
  } deferred_factory;

  SipFrameDemux demux(&connector, deferred_factory);
  demux_ptr = &demux;
  (void)demux_ptr;
  connector.attach_demux(&demux);

  // A cloud admin disabled/removed a subscriber → the cloud sends a
  // SUBSCRIBER_REVOKED frame carrying {societyId, sipUsername}. Tear down
  // that subscriber's live Asterisk calls via ARI. Malformed payloads are
  // logged-and-dropped — there is nothing to act on.
  demux.set_subscriber_revoked_handler(
      [&](const std::string &payload) {
        try {
          const auto j = nlohmann::json::parse(payload);
          if (j.contains("sipUsername") && j["sipUsername"].is_string())
            ari_client.revoke_subscriber(
                j["sipUsername"].get<std::string>());
        } catch (...) {
          ACE_ERROR((LM_ERROR,
                     ACE_TEXT("%D [pbx-agent] bad SUBSCRIBER_REVOKED "
                              "payload; dropped\n")));
        }
      });

  // Asterisk reported a SIP REGISTER state change → publish it to the
  // cloud's presence cache via a REGISTER_STATE SipFrame. Stream-id 0
  // (control op, not tied to any browser stream).
  ari_client.set_register_state_handler(
      [&](const std::string &sip_username, bool online) {
        const nlohmann::json payload = {
            {"societyId",   society_id},
            {"sipUsername", sip_username},
            {"online",      online},
        };
        connector.send_frame(SipFrame::Op::REGISTER_STATE, 0, payload.dump());
      });

  // After every (re)connect to the cloud, snapshot the agent's view of
  // every PJSIP endpoint into the cloud's presence cache.
  // `EndpointStateChange` only fires on transitions, so a tunnel drop
  // can leave the cache stale until the next register flip — which on
  // a quiet society might not happen for hours. The snapshot is the
  // resync that makes the directory's `online` flags accurate after a
  // reconnect.
  connector.set_on_connected([&]() {
    ari_client.publish_register_snapshot();
  });

  AsteriskWsFactory asterisk_factory(reactor, demux, ast_host,
                                       static_cast<std::uint16_t>(ast_port),
                                       "/ws");
  deferred_factory.real = &asterisk_factory;

  // ── Reconnect supervisor (1 s tick) ────────────────────────────────────
  ReconnectSupervisor supervisor(connector, ari_ws, reactor);
  if (supervisor.schedule_first_tick() == -1) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [pbx-agent] schedule_timer failed\n")));
    return -1;
  }

  // ── Subscriber change-stream tail (200 ms tick) ───────────────────────
  SubscriberWatcherTimer watcher_timer(watcher, reactor);
  if (watcher_timer.schedule_first_tick() == -1) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [pbx-agent] SubscriberWatcher schedule_timer "
                        "failed\n")));
    return -1;
  }

  ACE_DEBUG((LM_INFO,
             ACE_TEXT("%D [pbx-agent] reactor ready; entering event loop\n")));
  reactor->run_reactor_event_loop();

  ACE_DEBUG((LM_INFO, ACE_TEXT("%D [pbx-agent] shutdown clean\n")));
  return 0;
}
