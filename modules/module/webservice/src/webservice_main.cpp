#include "ace_https_client.hpp"
#include "cloud_tunnel_endpoint.hpp"
#include "emailservice.hpp"
#include "json.hpp"
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

// System wall-clock for VAPID JWT `exp` claim.
class SystemClock : public IClock {
public:
  std::int64_t now_unix() const override {
    return static_cast<std::int64_t>(std::time(nullptr));
  }
};

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
  ACE_LOG_MSG->priority_mask(LM_CRITICAL | LM_ERROR | LM_DEBUG,
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
    auto endpoint = std::make_unique<CloudTunnelEndpoint>();
    auto bridge   = std::make_unique<SipBridge>(endpoint.get());
    endpoint->attach_bridge(bridge.get());

    // Wire bridge → cloud-side dispatch hooks.
    //
    // TODO(layer-4-push): swap this for `PushSender::notify(subscriberId,
    // payload)` once VAPID keys + IPushHttpClient are wired from config.
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
    auto endpoint = std::make_unique<CloudTunnelEndpoint>();
    auto bridge   = std::make_unique<SipBridge>(endpoint.get());
    endpoint->attach_bridge(bridge.get());

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

    inst.start();
  }
  return 0;
}
