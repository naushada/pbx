#include "wsdbagent.hpp"

#include "ace/Get_Opt.h"
#include "ace/Log_Msg.h"
#include "ace/OS_NS_stdlib.h"

namespace {

void print_usage(const char *prog)
{
  ACE_ERROR((LM_ERROR,
             ACE_TEXT("Usage: %s [OPTIONS]\n\n"
                      "  --server-host  <host>   Heroku hostname (e.g. myapp.herokuapp.com)\n"
                      "  --server-port  <n>      Port (default: 443, TLS mandatory)\n"
                      ""
                      "  --tls-ca       <path>   CA certificate for verifying server cert (mTLS)\n"
                      "  --tls-cert     <path>   Client certificate (mTLS)\n"
                      "  --tls-key      <path>   Client private key (mTLS)\n"
                      "  --tls-hostname <name>   Expected server CN (default: --server-host)\n"
                      "  --mongo-db-uri <uri>    MongoDB connection URI\n"
                      "  --mongo-db-connection-pool <n>  Pool size (default: 10)\n"
                      "  --mongo-db-name <name>  Database name\n"
                      "  --backoff      <secs>   Reconnect wait in seconds (default: 5)\n"
                      "  --help                  Show this help\n"),
             prog));
}

} // namespace

int main(int argc, char *argv[])
{
  ACE_LOG_MSG->open(argv[0], ACE_LOG_MSG->STDERR | ACE_LOG_MSG->SYSLOG);
  ACE_LOG_MSG->priority_mask(LM_CRITICAL | LM_ERROR | LM_DEBUG,
                              ACE_Log_Msg::PROCESS);

  std::string server_host;
  int         server_port  = 0;
  std::string db_uri;
  std::string db_pool      = "10";
  std::string db_name;
  int         backoff      = 5;
  std::string tls_ca;
  std::string tls_cert;
  std::string tls_key;
  std::string tls_hostname;

  ACE_Get_Opt args(argc, argv, ACE_TEXT("H:p:U:C:D:b:A:E:K:N:nh"), 1);
  args.long_option(ACE_TEXT("server-host"),              'H', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("server-port"),              'p', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("mongo-db-uri"),             'U', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("mongo-db-connection-pool"), 'C', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("mongo-db-name"),            'D', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("backoff"),                  'b', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("tls-ca"),                   'A', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("tls-cert"),                 'E', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("tls-key"),                  'K', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("tls-hostname"),             'N', ACE_Get_Opt::ARG_REQUIRED);
  args.long_option(ACE_TEXT("no-ssl"),                   'n', ACE_Get_Opt::NO_ARG);
  args.long_option(ACE_TEXT("help"),                     'h', ACE_Get_Opt::NO_ARG);

  int c;
  while ((c = args()) != EOF) {
    switch (c) {
    case 'H': server_host = args.opt_arg(); break;
    case 'p': server_port = std::stoi(args.opt_arg()); break;
    case 'U': db_uri      = args.opt_arg(); break;
    case 'C': db_pool     = args.opt_arg(); break;
    case 'D': db_name     = args.opt_arg(); break;
    case 'b': backoff     = std::stoi(args.opt_arg()); break;
    case 'A': tls_ca      = args.opt_arg(); break;
    case 'E': tls_cert    = args.opt_arg(); break;
    case 'K': tls_key     = args.opt_arg(); break;
    case 'N': tls_hostname = args.opt_arg(); break;
    case 'n':
      ACE_ERROR((LM_ERROR, ACE_TEXT("--no-ssl is no longer supported. "
                                    "SSL/TLS is mandatory.\n")));
      return -1;
    case 'h': print_usage(argv[0]); return 0;
    case '?': print_usage(argv[0]); return -1;
    default: break;
    }
  }

  if (server_host.empty()) {
    ACE_ERROR((LM_ERROR, ACE_TEXT("--server-host is required\n")));
    print_usage(argv[0]);
    return -1;
  }
  if (db_uri.empty()) {
    ACE_ERROR((LM_ERROR, ACE_TEXT("--mongo-db-uri is required\n")));
    print_usage(argv[0]);
    return -1;
  }
  if (db_name.empty()) {
    ACE_ERROR((LM_ERROR, ACE_TEXT("--mongo-db-name is required\n")));
    print_usage(argv[0]);
    return -1;
  }

  if (server_port == 0) server_port = 443;

  ACE_DEBUG((LM_DEBUG,
             ACE_TEXT("%D [WsDbAgent] host=%s port=%d ssl=1 db=%s\n"),
             server_host.c_str(), server_port, db_name.c_str()));

  WsDbAgent agent(server_host,
                  static_cast<std::uint16_t>(server_port),
                  true,  // SSL is mandatory
                  db_uri, db_pool, db_name,
                  tls_ca, tls_cert, tls_key,
                  tls_hostname);
  agent.run(backoff);
  return 0;
}
