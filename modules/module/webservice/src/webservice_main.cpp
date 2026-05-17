#include "ace_https_client.hpp"
#include "cloud_tunnel_endpoint.hpp"
#include "cloud_tunnel_tick_driver.hpp"
#include "emailservice.hpp"
#include "json.hpp"
#include "presence_cache.hpp"
#include "push_sender.hpp"
#include "sip_bridge.hpp"
#include "webservice.hpp"
#include "wsdbproxy.hpp"

#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>

namespace {

using Arg = CommandArgumentName;
constexpr std::size_t idx(Arg a) { return static_cast<std::size_t>(a); }
constexpr std::size_t N = idx(Arg::MAX_CMD_ARG);

int opt_int(const std::array<std::string, N> &opt, Arg key, int default_val) {
  const auto &s = opt[idx(key)];
  return s.empty() ? default_val : std::stoi(s);
}

// System wall-clock — shared by VAPID JWT `exp` (PushSender) and the
// cloud-side SipFrame heartbeat (CloudTunnelEndpoint).
class SystemClock : public IClock {
public:
  std::int64_t now_unix() const override {
    return static_cast<std::int64_t>(std::time(nullptr));
  }
};

// The cloud-side SipFrame heartbeat (`CloudTunnelEndpoint::tick`) is
// driven by `CloudTunnelTickDriver` (modules/module/pbx/inc/
// cloud_tunnel_tick_driver.hpp), which is unit-tested in isolation
// and replaces the previously-inline `CloudTunnelHeartbeatTimer`.

std::string read_file_to_string(const std::string &path) {
  std::ifstream f(path);
  if (!f.is_open()) return {};
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

/// Construct a PushSender (or nullptr if VAPID config is incomplete) and
/// return both the sender + its dependencies so the caller can keep them
/// alive for the WebServer's lifetime.
struct PushDeps {
  std::unique_ptr<AceHttpsClient> http;
  std::unique_ptr<SystemClock>    clock;
  std::unique_ptr<PushSender>     sender;
};

PushDeps make_push_deps(const std::array<std::string, N> &opt,
                         IMongodbClient *db_ptr) {
  PushDeps deps;
  const std::string &key_path = opt[idx(Arg::VAPID_KEY_PATH)];
  const std::string &subject  = opt[idx(Arg::VAPID_SUBJECT)];
  if (key_path.empty() || subject.empty() || !db_ptr) {
    ACE_DEBUG((LM_WARNING,
               ACE_TEXT("%D [pbx-cloud] PushSender disabled: "
                        "--vapid-key-path=%s --vapid-subject=%s db=%p\n"),
               key_path.c_str(), subject.c_str(),
               static_cast<void *>(db_ptr)));
    return deps;
  }
  const std::string pem = read_file_to_string(key_path);
  if (pem.empty()) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [pbx-cloud] PushSender: failed to read VAPID "
                        "key file '%s'; PUSH_NOTIFY will be log-only\n"),
               key_path.c_str()));
    return deps;
  }

  PushSender::Config cfg;
  cfg.vapid_private_pem = pem;
  cfg.vapid_subject     = subject;

  deps.http   = std::make_unique<AceHttpsClient>();
  deps.clock  = std::make_unique<SystemClock>();
  deps.sender = std::make_unique<PushSender>(std::move(cfg), *deps.http,
                                              *db_ptr, *deps.clock);
  ACE_DEBUG((LM_INFO,
             ACE_TEXT("%D [pbx-cloud] PushSender ready (subject=%s, "
                      "public key=%s)\n"),
             subject.c_str(), deps.sender->vapid_public_b64url().c_str()));
  return deps;
}

void print_usage(const char *prog) {
  ACE_ERROR((LM_ERROR,
             ACE_TEXT("Usage: %s [OPTIONS]\n\n"
                      "  --server-ip              <addr>  Bind address (default: all interfaces)\n"
                      "  --server-port            <n>     Listen port            (default: 8080)\n"
                      "  --server-worker          <n>     Worker thread count    (default: 10)\n"
                      "  --mongo-db-uri           <uri>   MongoDB connection URI (local mode only)\n"
                      "  --mongo-db-connection-pool <n>   Connection pool size   (local mode only, default: 50)\n"
                      "  --mongo-db-name          <name>  Database name          (local mode only)\n"
                      "  --email-from-name        <name>  Outgoing email display name\n"
                      "  --email-from-id          <addr>  Outgoing email address\n"
                      "  --email-from-password    <pw>    Outgoing email password\n"
                      "  --remote-db                      Use WebSocket DB proxy (ws-db-agent)\n"
                      "  --agent-port             <n>     Dedicated mTLS port for ws-db-agent\n"
                      "  --tls-cert               <path>  Server certificate (PEM, mTLS mode)\n"
                      "  --tls-key                <path>  Server private key  (PEM, mTLS mode)\n"
                      "  --tls-ca                 <path>  CA cert for verifying agent cert (PEM)\n"
                      "  --vapid-key-path         <path>  ECDSA P-256 PEM for Web Push (RFC 8292); if unset, PUSH_NOTIFY is logged but not sent\n"
                      "  --vapid-subject          <uri>   mailto:ops@... for the VAPID `sub` claim\n"
                      "  --migrate-passwords              Hash plain-text account passwords and exit\n"
"  --help                           Show this help\n"),
             prog));
}

} // namespace

int main(int argc, char *argv[]) {
  ACE_LOG_MSG->open(argv[0], ACE_LOG_MSG->STDERR | ACE_LOG_MSG->SYSLOG);
  // ACE log levels are independent bits — you have to OR every one
  // you want to see. LM_INFO + LM_WARNING were missing for months,
  // which silently dropped every ACE_DEBUG((LM_INFO, ...)) line —
  // the entire observability story of PRs #80, #85, #88 was
  // invisible until this fix. Match pbx-agent's mask shape.
  ACE_LOG_MSG->priority_mask(LM_CRITICAL | LM_ERROR | LM_WARNING |
                              LM_DEBUG | LM_INFO,
                             ACE_Log_Msg::PROCESS);

  std::array<std::string, N> opt{};

  ACE_Get_Opt args(argc, argv, ACE_TEXT("s:p:w:u:c:d:n:i:o:a:e:k:q:V:S:rhm"), 1);
  args.long_option(ACE_TEXT("server-ip"),               's', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("server-port"),             'p', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("server-worker"),           'w', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("mongo-db-uri"),            'u', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("mongo-db-connection-pool"),'c', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("mongo-db-name"),           'd', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("email-from-name"),         'n', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("email-from-id"),           'i', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("email-from-password"),     'o', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("remote-db"),               'r', ACE_Get_Opt::NO_ARG);
  args.long_option(ACE_TEXT("agent-port"),              'a', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("tls-cert"),                'e', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("tls-key"),                 'k', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("tls-ca"),                  'q', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("migrate-passwords"),       'm', ACE_Get_Opt::NO_ARG);
  args.long_option(ACE_TEXT("vapid-key-path"),          'V', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("vapid-subject"),           'S', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("help"),                    'h', ACE_Get_Opt::NO_ARG);

  // Short-option char → enum key table
  static constexpr std::pair<char, Arg> kOptMap[] = {
    {'s', Arg::SERVER_IP},
    {'p', Arg::SERVER_PORT},
    {'w', Arg::SERVER_WORKER_NODE},
    {'u', Arg::DB_URI},
    {'c', Arg::DB_CONN_POOL},
    {'d', Arg::DB_NAME},
    {'n', Arg::EMAIL_FROM_NAME},
    {'i', Arg::EMAIL_FROM_ID},
    {'o', Arg::EMAIL_FROM_PASSWORD},
    {'a', Arg::AGENT_PORT},
    {'e', Arg::TLS_CERT},
    {'k', Arg::TLS_KEY},
    {'q', Arg::TLS_CA},
    {'V', Arg::VAPID_KEY_PATH},
    {'S', Arg::VAPID_SUBJECT},
  };

  int c;
  while ((c = args()) != EOF) {
    if (c == 'h') { print_usage(argv[0]); return 0; }
    if (c == '?') { print_usage(argv[0]); return -1; }
    if (c == 'm') { opt[idx(Arg::MIGRATE_PASSWORDS)] = "1"; continue; }
    if (c == 'r') { opt[idx(Arg::REMOTE_DB)] = "1"; continue; }

    for (auto &[ch, key] : kOptMap) {
      if (c == ch) {
        opt[idx(key)] = args.opt_arg();
        ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [WebServer:%t] %M %N:%l opt -%c = %s\n"),
                   c, opt[idx(key)].c_str()));
        break;
      }
    }
  }

  const int port      = opt_int(opt, Arg::SERVER_PORT,        8080);
  const int worker    = opt_int(opt, Arg::SERVER_WORKER_NODE, 10);
  const bool remoteDb = !opt[idx(Arg::REMOTE_DB)].empty();
  const bool migrate  = !opt[idx(Arg::MIGRATE_PASSWORDS)].empty();

  if (migrate) {
    if (remoteDb) {
      ACE_ERROR((LM_ERROR, ACE_TEXT("--migrate-passwords requires local MongoDB\n")));
      return -1;
    }
    const std::string &uri = opt[idx(Arg::DB_URI)];
    if (uri.empty()) {
      ACE_ERROR((LM_ERROR,
                 ACE_TEXT("--migrate-passwords requires --mongo-db-uri\n")));
      return -1;
    }
    MongodbClient db(uri);
    int migrated = migrate_account_passwords(db);
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("Migration complete: %d account(s) updated\n"),
               migrated));
    return 0;
  }

  ACE_DEBUG((LM_DEBUG,
             ACE_TEXT("%D [WebServer:%t] %M %N:%l "
                      "port:%d workers:%d remote-db:%d db-pool:%s db-name:%s\n"),
             port, worker, (int)remoteDb,
             opt[idx(Arg::DB_CONN_POOL)].c_str(),
             opt[idx(Arg::DB_NAME)].c_str()));

  SMTP::Account::instance().from_name(opt[idx(Arg::EMAIL_FROM_NAME)]);
  SMTP::Account::instance().from_email(opt[idx(Arg::EMAIL_FROM_ID)]);
  SMTP::Account::instance().from_password(opt[idx(Arg::EMAIL_FROM_PASSWORD)]);

  if (remoteDb) {
    const std::string &agentPort = opt[idx(Arg::AGENT_PORT)];
    const std::string &tlsCert   = opt[idx(Arg::TLS_CERT)];
    const std::string &tlsKey    = opt[idx(Arg::TLS_KEY)];
    const std::string &tlsCa     = opt[idx(Arg::TLS_CA)];

    std::unique_ptr<WsDbServer> wsServer;
    if (!agentPort.empty() && !tlsCert.empty() && !tlsKey.empty() && !tlsCa.empty()) {
      WsDbServer::TlsConfig tls;
      tls.port = static_cast<std::uint16_t>(std::stoi(agentPort));
      tls.cert = tlsCert;
      tls.key  = tlsKey;
      tls.ca   = tlsCa;
      wsServer = std::make_unique<WsDbServer>(std::move(tls));
      ACE_DEBUG((LM_DEBUG,
                 ACE_TEXT("%D [WebServer:%t] %M %N:%l "
                          "mTLS agent acceptor on port %s\n"), agentPort.c_str()));
    } else if (!tlsCert.empty() && !tlsKey.empty()) {
      wsServer = std::make_unique<WsDbServer>(tlsCert, tlsKey, tlsCa);
      ACE_DEBUG((LM_DEBUG,
                 ACE_TEXT("%D [WebServer:%t] %M %N:%l "
                          "Heroku mode with inner TLS (cert=%s)\n"),
                 tlsCert.c_str()));
    } else {
      ACE_ERROR((LM_WARNING,
                 ACE_TEXT("%D [WebServer:%t] %M %N:%l "
                          "--tls-cert/--tls-key not set — inner TLS disabled "
                          "(agent traffic will not be encrypted)\n")));
      wsServer = std::make_unique<WsDbServer>();
    }

    auto proxy = std::make_unique<WsMongodbProxy>(*wsServer);
    WebServer inst(opt[idx(Arg::SERVER_IP)], port, worker,
                   std::move(proxy), std::move(wsServer));

    // ── onprem-pbx control plane bootstrap (remote-db mode) ─────────────
    // CloudTunnelEndpoint accepts the on-prem agent's /agent WS upgrade
    // (see WebConnection::handle_input). SipBridge multiplexes every
    // browser /sip-ws session onto that single tunnel. They share the
    // WebServer's MongoDB client for cdr/push-subscriptions writes.
    // The clock is wall-time; both the heartbeat and (later) PushSender
    // share it. It outlives the WebServer move below.
    SystemClock cte_clock;
    CloudTunnelEndpoint::Config cte_cfg;
    // Reuse the same --tls-cert / --tls-key / --tls-ca triple that
    // already drives /ws/db's InnerTlsServer — same CA, same trust
    // boundary. Heroku terminates the outer TLS at the router, so
    // without this layer the agent's mTLS would be theatre.
    cte_cfg.inner_tls.cert_path = tlsCert;
    cte_cfg.inner_tls.key_path  = tlsKey;
    cte_cfg.inner_tls.ca_path   = tlsCa;
    auto endpoint = std::make_unique<CloudTunnelEndpoint>(cte_cfg, &cte_clock);
    auto bridge   = std::make_unique<SipBridge>(endpoint.get());
    auto presence = std::make_unique<InMemoryPresenceCache>();
    endpoint->attach_bridge(bridge.get());

    // REGISTER_STATE: agent reported a SIP REGISTER state change. Capture
    // the cache by raw ptr — the unique_ptr below moves ownership into
    // `inst` but the underlying object stays at the same address.
    {
      IPresenceCache *cache = presence.get();
      bridge->set_register_state_handler(
          [cache](const std::string &payload) {
            try {
              const auto j = nlohmann::json::parse(payload);
              if (!j.contains("societyId")   || !j["societyId"].is_string() ||
                  !j.contains("sipUsername") || !j["sipUsername"].is_string() ||
                  !j.contains("online")      || !j["online"].is_boolean()) return;
              cache->set(j["societyId"].get<std::string>(),
                         j["sipUsername"].get<std::string>(),
                         j["online"].get<bool>());
            } catch (const std::exception &e) {
              ACE_ERROR((LM_ERROR,
                         ACE_TEXT("%D [pbx-cloud] register-state handler: %s\n"),
                         e.what()));
            }
          });
    }

    // AGENT_HELLO: agent identified itself by societyId on (re)connect.
    // Look up the society doc, emit SOCIETY_BOOTSTRAP carrying the
    // canonical sipRealm. Capture the bridge raw ptr — same reasoning
    // as the cache above; the unique_ptr below moves ownership into
    // `inst` but the bridge stays put.
    {
      SipBridge      *brg = bridge.get();
      IMongodbClient *db  = inst.mongodbcInst();
      brg->set_agent_hello_handler(
          [brg, db](const std::string &payload) {
            if (!db) return;
            std::string society_id;
            try {
              const auto j = nlohmann::json::parse(payload);
              if (!j.contains("societyId") || !j["societyId"].is_string()) return;
              society_id = j["societyId"].get<std::string>();
            } catch (const std::exception &e) {
              ACE_ERROR((LM_ERROR,
                         ACE_TEXT("%D [pbx-cloud] AGENT_HELLO parse: %s\n"),
                         e.what()));
              brg->bootstrap_society("", "");  // ACK silently with empty payload
              return;
            }

            // Look up by `code` — handle_society_POST writes societies
            // with an auto-generated ObjectId _id, so _id-based queries
            // never match the operator-supplied society label.
            const std::string doc = db->get_document(
                "societies",
                R"({"code":")" + society_id + R"("})",
                "{}");
            std::string sip_realm;
            if (!doc.empty()) {
              try {
                sip_realm = nlohmann::json::parse(doc)
                                .value("sipRealm", std::string{});
              } catch (const std::exception &e) {
                ACE_ERROR((LM_ERROR,
                           ACE_TEXT("%D [pbx-cloud] AGENT_HELLO society doc "
                                    "parse: %s\n"), e.what()));
              }
            } else {
              ACE_ERROR((LM_ERROR,
                         ACE_TEXT("%D [pbx-cloud] AGENT_HELLO societyId=%s "
                                  "not found — sending ACK with empty "
                                  "sipRealm (agent will fall back to "
                                  "--sip-realm CLI default)\n"),
                         society_id.c_str()));
            }

            // ALWAYS send SOCIETY_BOOTSTRAP back, even on a lookup
            // miss. Two reasons:
            //   1. Heroku's router appears to close /agent connections
            //      that have no server→client traffic (suspected; see
            //      project_agent_heroku_3s_disconnect memory).
            //   2. Agent gets an "I was acknowledged" signal separate
            //      from the raw inner-TLS handshake.
            // The agent's existing handler treats empty sipRealm as
            // "use your --sip-realm CLI fallback".
            brg->bootstrap_society(society_id, sip_realm);
          });
    }

    // Wire bridge → cloud-side dispatch hooks.
    //
    // If VAPID is configured, instantiate PushSender and route the
    // bridge's push handler through it. The handler payload is the
    // PUSH_NOTIFY JSON the agent shipped:
    //   {"subscriberId":"u1","callerFlat":"A-101","callId":"abc"}
    // PushSender::notify looks up `push_subscriptions` by subscriberId
    // and POSTs an encrypted VAPID notification to each endpoint.
    PushDeps push_deps = make_push_deps(opt, inst.mongodbcInst());
    if (push_deps.sender) {
      PushSender *sender = push_deps.sender.get();
      bridge->set_push_notify_handler(
          [sender](const std::string &payload) {
            try {
              const auto j = nlohmann::json::parse(payload);
              if (!j.contains("subscriberId") ||
                  !j["subscriberId"].is_string()) return;
              const std::string sub_id =
                  j["subscriberId"].get<std::string>();
              sender->notify(sub_id, payload);
            } catch (const std::exception &e) {
              ACE_ERROR((LM_ERROR,
                         ACE_TEXT("%D [pbx-cloud] push handler: %s\n"),
                         e.what()));
            }
          });
      ACE_DEBUG((LM_INFO,
                 ACE_TEXT("%D [pbx-cloud] PUSH_NOTIFY → PushSender wired\n")));
    } else {
      bridge->set_push_notify_handler(
          [](const std::string &payload) {
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D [pbx-cloud] PUSH_NOTIFY received "
                                "(PushSender not configured — see "
                                "--vapid-key-path / --vapid-subject): %s\n"),
                       payload.c_str()));
          });
    }

    // CDR_PUSH: the agent finalized a CDR doc and is shipping it up.
    // Write into the `cdr` collection via the WebServer's MongodbClient.
    {
      IMongodbClient *db_ptr = inst.mongodbcInst();
      bridge->set_cdr_push_handler(
          [db_ptr](const std::string &payload) {
            if (!db_ptr) return;
            // The agent ships the doc as JSON (CDR document, not BSON —
            // BSON conversion happens in MongodbClient::create_document).
            db_ptr->create_document(db_ptr->get_database(), "cdr", payload);
          });
    }

    inst.setCloudTunnelEndpoint(std::move(endpoint));
    inst.setSipBridge(std::move(bridge));
    inst.setPresenceCache(std::move(presence));

    // Arm the SipFrame heartbeat tick (DESIGN.md §6.6 +
    // docs/design/operations/cloud-tunnel-liveness.md) — the only
    // periodic work the cloud side does. Schedule before start()
    // blocks. Tick at 5 s matches the heartbeat_interval_sec
    // default; faster ticks would short-circuit inside maybe_heartbeat
    // anyway.
    CloudTunnelTickDriver cte_tick_driver(*inst.cloudTunnelEndpoint(),
                                            /*interval_sec=*/5);
    if (cte_tick_driver.register_with_reactor(
            ACE_Reactor::instance()) == -1) {
      ACE_ERROR((LM_ERROR,
                 ACE_TEXT("%D [pbx-cloud] cloud-tunnel tick driver "
                          "schedule_timer failed\n")));
    }

    inst.start();
  } else {
    WebServer inst(opt[idx(Arg::SERVER_IP)], port, worker,
                   opt[idx(Arg::DB_URI)],
                   opt[idx(Arg::DB_CONN_POOL)],
                   opt[idx(Arg::DB_NAME)]);

    // ── onprem-pbx control plane bootstrap (local-db mode) ──────────────
    // Mirror of the block above. The agent dial-in path is the same in
    // both modes — what differs is where the cloud's Mongo data lives
    // (local Mongo vs. remote on-prem Mongo via wsdbagent).
    SystemClock cte_clock;
    CloudTunnelEndpoint::Config cte_cfg;
    // Same --tls-cert / --tls-key / --tls-ca triple reused for /agent
    // InnerTLS — Heroku terminates the outer TLS at the router so this
    // is the real mTLS trust boundary. Same convention as the
    // remote-db block above.
    cte_cfg.inner_tls.cert_path = opt[idx(Arg::TLS_CERT)];
    cte_cfg.inner_tls.key_path  = opt[idx(Arg::TLS_KEY)];
    cte_cfg.inner_tls.ca_path   = opt[idx(Arg::TLS_CA)];
    auto endpoint = std::make_unique<CloudTunnelEndpoint>(cte_cfg, &cte_clock);
    auto bridge   = std::make_unique<SipBridge>(endpoint.get());
    auto presence = std::make_unique<InMemoryPresenceCache>();
    endpoint->attach_bridge(bridge.get());

    // REGISTER_STATE: agent reported a SIP REGISTER state change. Same
    // wiring as the remote-db block above; cache moved into `inst` below.
    {
      IPresenceCache *cache = presence.get();
      bridge->set_register_state_handler(
          [cache](const std::string &payload) {
            try {
              const auto j = nlohmann::json::parse(payload);
              if (!j.contains("societyId")   || !j["societyId"].is_string() ||
                  !j.contains("sipUsername") || !j["sipUsername"].is_string() ||
                  !j.contains("online")      || !j["online"].is_boolean()) return;
              cache->set(j["societyId"].get<std::string>(),
                         j["sipUsername"].get<std::string>(),
                         j["online"].get<bool>());
            } catch (const std::exception &e) {
              ACE_ERROR((LM_ERROR,
                         ACE_TEXT("%D [pbx-cloud] register-state handler: %s\n"),
                         e.what()));
            }
          });
    }

    // AGENT_HELLO: same as the remote-db block above — look up the
    // society doc and bootstrap the agent with its sipRealm.
    {
      SipBridge      *brg = bridge.get();
      IMongodbClient *db  = inst.mongodbcInst();
      brg->set_agent_hello_handler(
          [brg, db](const std::string &payload) {
            if (!db) return;
            std::string society_id;
            try {
              const auto j = nlohmann::json::parse(payload);
              if (!j.contains("societyId") || !j["societyId"].is_string()) return;
              society_id = j["societyId"].get<std::string>();
            } catch (const std::exception &e) {
              ACE_ERROR((LM_ERROR,
                         ACE_TEXT("%D [pbx-cloud] AGENT_HELLO parse: %s\n"),
                         e.what()));
              brg->bootstrap_society("", "");  // ACK silently with empty payload
              return;
            }

            // Look up by `code` — handle_society_POST writes societies
            // with an auto-generated ObjectId _id, so _id-based queries
            // never match the operator-supplied society label.
            const std::string doc = db->get_document(
                "societies",
                R"({"code":")" + society_id + R"("})",
                "{}");
            std::string sip_realm;
            if (!doc.empty()) {
              try {
                sip_realm = nlohmann::json::parse(doc)
                                .value("sipRealm", std::string{});
              } catch (const std::exception &e) {
                ACE_ERROR((LM_ERROR,
                           ACE_TEXT("%D [pbx-cloud] AGENT_HELLO society doc "
                                    "parse: %s\n"), e.what()));
              }
            } else {
              ACE_ERROR((LM_ERROR,
                         ACE_TEXT("%D [pbx-cloud] AGENT_HELLO societyId=%s "
                                  "not found — sending ACK with empty "
                                  "sipRealm (agent will fall back to "
                                  "--sip-realm CLI default)\n"),
                         society_id.c_str()));
            }

            // ALWAYS send SOCIETY_BOOTSTRAP back, even on a lookup
            // miss. Two reasons:
            //   1. Heroku's router appears to close /agent connections
            //      that have no server→client traffic (suspected; see
            //      project_agent_heroku_3s_disconnect memory).
            //   2. Agent gets an "I was acknowledged" signal separate
            //      from the raw inner-TLS handshake.
            // The agent's existing handler treats empty sipRealm as
            // "use your --sip-realm CLI fallback".
            brg->bootstrap_society(society_id, sip_realm);
          });
    }

    // If VAPID is configured, instantiate PushSender and route the
    // bridge's push handler through it. The handler payload is the
    // PUSH_NOTIFY JSON the agent shipped:
    //   {"subscriberId":"u1","callerFlat":"A-101","callId":"abc"}
    // PushSender::notify looks up `push_subscriptions` by subscriberId
    // and POSTs an encrypted VAPID notification to each endpoint.
    PushDeps push_deps = make_push_deps(opt, inst.mongodbcInst());
    if (push_deps.sender) {
      PushSender *sender = push_deps.sender.get();
      bridge->set_push_notify_handler(
          [sender](const std::string &payload) {
            try {
              const auto j = nlohmann::json::parse(payload);
              if (!j.contains("subscriberId") ||
                  !j["subscriberId"].is_string()) return;
              const std::string sub_id =
                  j["subscriberId"].get<std::string>();
              sender->notify(sub_id, payload);
            } catch (const std::exception &e) {
              ACE_ERROR((LM_ERROR,
                         ACE_TEXT("%D [pbx-cloud] push handler: %s\n"),
                         e.what()));
            }
          });
      ACE_DEBUG((LM_INFO,
                 ACE_TEXT("%D [pbx-cloud] PUSH_NOTIFY → PushSender wired\n")));
    } else {
      bridge->set_push_notify_handler(
          [](const std::string &payload) {
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D [pbx-cloud] PUSH_NOTIFY received "
                                "(PushSender not configured — see "
                                "--vapid-key-path / --vapid-subject): %s\n"),
                       payload.c_str()));
          });
    }

    {
      IMongodbClient *db_ptr = inst.mongodbcInst();
      bridge->set_cdr_push_handler(
          [db_ptr](const std::string &payload) {
            if (!db_ptr) return;
            db_ptr->create_document(db_ptr->get_database(), "cdr", payload);
          });
    }

    inst.setCloudTunnelEndpoint(std::move(endpoint));
    inst.setSipBridge(std::move(bridge));
    inst.setPresenceCache(std::move(presence));

    // Arm the SipFrame heartbeat tick (DESIGN.md §6.6 +
    // docs/design/operations/cloud-tunnel-liveness.md) — the only
    // periodic work the cloud side does. Schedule before start()
    // blocks. Tick at 5 s matches the heartbeat_interval_sec
    // default; faster ticks would short-circuit inside maybe_heartbeat
    // anyway.
    CloudTunnelTickDriver cte_tick_driver(*inst.cloudTunnelEndpoint(),
                                            /*interval_sec=*/5);
    if (cte_tick_driver.register_with_reactor(
            ACE_Reactor::instance()) == -1) {
      ACE_ERROR((LM_ERROR,
                 ACE_TEXT("%D [pbx-cloud] cloud-tunnel tick driver "
                          "schedule_timer failed\n")));
    }

    inst.start();
  }
  return 0;
}
