#include "emailservice.hpp"
#include "webservice.hpp"
#include "wsdbproxy.hpp"

namespace {

using Arg = CommandArgumentName;
constexpr std::size_t idx(Arg a) { return static_cast<std::size_t>(a); }
constexpr std::size_t N = idx(Arg::MAX_CMD_ARG);

int opt_int(const std::array<std::string, N> &opt, Arg key, int default_val) {
  const auto &s = opt[idx(key)];
  return s.empty() ? default_val : std::stoi(s);
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

  ACE_Get_Opt args(argc, argv, ACE_TEXT("s:p:w:u:c:d:n:i:o:a:e:k:q:rhm"), 1);
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
    inst.start();
  } else {
    WebServer inst(opt[idx(Arg::SERVER_IP)], port, worker,
                   opt[idx(Arg::DB_URI)],
                   opt[idx(Arg::DB_CONN_POOL)],
                   opt[idx(Arg::DB_NAME)]);
    inst.start();
  }
  return 0;
}
