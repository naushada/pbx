#ifndef EMAILSERVICE_HPP
#define EMAILSERVICE_HPP

#include <vector>
#include <iostream>
#include <unordered_map>
#include <variant>
#include <type_traits>
#include <optional>

#include "ace/Reactor.h"
#include "ace/Basic_Types.h"
#include "ace/Event_Handler.h"
#include "ace/Task.h"
#include "ace/INET_Addr.h"
#include "ace/SOCK_Stream.h"
#include "ace/SOCK_Acceptor.h"
#include "ace/Task_T.h"
#include "ace/Timer_Queue_T.h"
#include "ace/OS_Memory.h"
#include "ace/Thread_Manager.h"
#include "ace/Get_Opt.h"
#include "ace/Signal.h"
#include "ace/SSL/SSL_SOCK.h"
#include "ace/SSL/SSL_SOCK_Stream.h"
#include "ace/SSL/SSL_SOCK_Connector.h"
#include "ace/Semaphore.h"
#include "ace/Codecs.h"


namespace SMTP {
    struct Response {
        std::uint32_t m_reply;
        std::string m_statusCode;
        bool operator==(const Response& rp) const {
            return(m_reply == rp.m_reply);
        }

    Response() = default;
    ~Response() = default;
    
    Response(std::uint32_t reply, std::string code) {
        m_reply = reply;
        m_statusCode = code;
    }
  };

  struct hFn {
    auto operator()(const Response& r1) const {
        std::size_t h1 = std::hash<std::uint32_t>()(r1.m_reply);
        std::size_t h2 = std::hash<std::string>()(r1.m_statusCode);
 
        return (h1 ^ h2);
    }
  };

  std::uint32_t parseSmtpCommand(const std::string& in, std::unordered_map<Response, std::string, hFn>& out);
  SMTP::Response getSmtpStatusCode(const std::string& in);
  std::uint32_t getBase64(const std::string& in, std::string& b64Out);
  auto find(const std::string& in, const std::string& what);
  void display(const std::string& in);
  std::string getContentType(const std::string& ext);

  enum reply_code: std::uint32_t {
    REPLY_CODE_214_Response_to_HELP = 214,
    REPLY_CODE_211_System_status = 211,
    REPLY_CODE_220_Service_ready = 220,
    REPLY_CODE_221_Service_closing_transmission_channel = 221,
    //REPLY_CODE_221_2_0_0_Goodbye = 211,
    REPLY_CODE_235_2_7_0_Authentication_succeeded = 235,
    REPLY_CODE_240_Quit = 240,
    REPLY_CODE_250_Request_mail_action_okay_completed = 250,
    REPLY_CODE_251_User_not_local_will_forward = 251,
    REPLY_CODE_252_Can_not_verify_the_user = 252,
    REPLY_CODE_334_Server_challenge = 334,
    //REPLY_CODE_354_Start_mail_input = 354,
    /* -ve status code */
    REPLY_CODE_432_4_7_12_A_password_transition_needed = 432,
    REPLY_CODE_421_Service_not_available_closing_channel = 421,
    REPLY_CODE_450_Requested_mail_action_not_taken = 450,
    REPLY_CODE_451_4_4_1_IMAP_server_unavailable = 451,
    //REPLY_CODE_451_Requested_action_aborted = 451,
    REPLY_CODE_452_Requested_action_not_taken = 451,
    REPLY_CODE_454_4_7_0_Temporary_authentication_failure = 454,
    REPLY_CODE_455_Server_unable_to_accomodate_parameters = 455,
    /* -ve status code */
    REPLY_CODE_500_Syntax_error_command_unrecognized = 500,
    REPLY_CODE_500_5_5_6_Authentication_exchange_line_too_long = 500,
    REPLY_CODE_501_Syntax_error_in_parameters = 501,
    //REPLY_CODE_501_5_5_2_Cannot_base64_decode_client_response = 501,
    //REPLY_CODE_501_5_7_0_Client_initiated_authentication_exchange = 501,
    REPLY_CODE_502_Command_not_implemented = 502,
    REPLY_CODE_503_Bad_sequence_of_command = 503,
    REPLY_CODE_504_Command_parameter_is_not_implemented = 504,
    //REPLY_CODE_504_5_5_4_Unrecognized_authentication_type = 504,
    REPLY_CODE_521_Server_does_not_accept_mail = 521,
    REPLY_CODE_523_Encryption_needed = 523,
    REPLY_CODE_530_5_7_0_Authentication_needed = 530,
    REPLY_CODE_534_5_7_9_Authentication_mechanism_is_too_weak = 534,
    REPLY_CODE_535_5_7_8_Authentication_credentials_invalid = 535,
    REPLY_CODE_538_5_7_11_Encryption_required = 538,
    REPLY_CODE_550_Requested_action_not_taken = 550,
    REPLY_CODE_551_User_not_local = 551,
    REPLY_CODE_552_Requested_mail_action_aborted = 552,
    REPLY_CODE_553_Requested_action_not_taken = 553,
    REPLY_CODE_554_Transaction_has_failed = 554,
    //REPLY_CODE_554_5_3_4_Message_too_big_for_system = 554,
    REPLY_CODE_556_Domain_does_not_accept_mail = 556

  };

  enum status_code : std::uint32_t {
      OK = 0,
      TLS_NOT_SUPPORTED = 1,
      END_EMAIL_TRANSACTION,
      REMAIN_IN_SAME_STATE,
      GOTO_NEXT_STATE,
      /* Expecting Username: for challenge from SMTP server */
      CHALLENGE_FOR_USERNAME_FAILED,
      CHALLENGE_FOR_PASSWORD_FAILED,
      BASE64_DECODING_FAILED,
      INCORRECT_LOGIN_CREDENTIALS,
      ERROR_END
  };

  //std::unordered_map<statusCode_t, std::string>statusCodeUMap;
  /// @brief Forward declaration of classes
  class User;
  class HELO;
  class MAIL;
  class RCPT;
  class DATA;
  class QUIT;
  class BODY;
  class HELP;
  class NOOP;
  class VRFY;
  class EXPN;
  class RESET;
  class GREETING;

  /// @brief For new state, add into this variant 
  using States = std::variant<GREETING, HELO, MAIL, RCPT, DATA, QUIT, BODY, HELP, NOOP, VRFY, EXPN, RESET>;
  
  class Tls {
      public:

          Tls(User *usr);
          ~Tls();

          void init();
          std::int32_t start(std::int32_t handle);
          std::int32_t read(std::string& plain_buffer);
          std::int32_t write(const std::string& plain_buffer);
          std::int32_t peek(std::string& plain_buffer);
          void close();

          User& user() {return(*m_user);}

          bool isTlsUP() const {
            return(m_isTlsUP);
          }
          void isTlsUP(bool val) {
            m_isTlsUP = val;
          }

      private:
          SSL *m_ssl;
          SSL_CTX *m_sslCtx;
          User *m_user;
          bool m_isTlsUP = false;
  };

  /// @brief the Client instance will be active object
  class Client : public ACE_Task<ACE_MT_SYNCH> {

    public:
      Client(ACE_UINT16 port, std::string smtpAddress, User* user);
      ~Client();

      int svc(void) override;
      int open(void *args=0) override;
      int close (u_long flags=0) override;

      ACE_INT32 handle_timeout(const ACE_Time_Value &tv, const void *act=0) override;
      ACE_INT32 handle_input(ACE_HANDLE handle) override;
      ACE_INT32 handle_signal(int signum, siginfo_t *s = 0, ucontext_t *u = 0) override;
      ACE_INT32 handle_close (ACE_HANDLE = ACE_INVALID_HANDLE, ACE_Reactor_Mask = 0) override;
      ACE_HANDLE get_handle() const override;
      void start();
      void stop();
      void main();

      std::int32_t tx(const std::string& in);
      std::int32_t rx(std::string &out);
      User& user() {
        return(*m_user);
      }

      ACE_INET_Addr m_smtpServerAddress;
      ACE_SOCK_Connector m_connection;
      ACE_SOCK_Stream m_stream;
      ACE_Message_Block *m_mb;
      bool m_mailServiceAvailable;
      User *m_user;
      ACE_Sig_Set ss;
      std::unique_ptr<ACE_Semaphore> m_semaphore;
  };

  /*  _ ___
   * ||   ||
   * ||_ _//
   * ||   \\
   * ||    \\
   */
  template<typename ST>
  class Transaction {
    public:

      Transaction() = default;
      ~Transaction() = default;

      template<typename... Args>
      void set_state(auto new_state, Args... args) {

        std::visit([&](auto st) -> void {
            st.onExit(args...);
          }, m_state);

        m_state = std::move(new_state);

        std::visit([&](auto st) -> void {
            st.onEntry(args...);
          }, m_state);
      }

      std::uint32_t onRx(const std::string& in, std::string& out, ST& new_state, User& parent)
      {
          std::int32_t ret_status;
          std::visit([&](auto&& active_st) -> void {
              ret_status = active_st.onResponse(in, out, new_state, parent);
          }, m_state);
        return(ret_status);
      }

    private:
      ST m_state;
  };

  class GREETING {
    public:
      GREETING() = default;
      ~GREETING() = default;

      void onEntry();
      void onExit();
      std::uint32_t onResponse(const std::string& in);
      std::uint32_t onResponse();
      std::uint32_t onCommand(const std::string& in, std::string& out, States& new_state);
      std::uint32_t onResponse(const std::string& in, std::string& out, States& new_state, User& parent);

  };

  class MAIL {
    enum AuthSteps : std::uint32_t {
      AUTH_INIT,
      AUTH_USRNAME,
      AUTH_PASSWORD,
      AUTH_SUCCESS,
      AUTH_FAILURE
    };

    AuthSteps m_authStage;

    public:
      MAIL() : m_authStage(AUTH_INIT) {

      }

      ~MAIL() = default;

      void onEntry();
      void onExit();
      std::uint32_t onResponse(const std::string& in);
      std::uint32_t onResponse();
      std::uint32_t onResponse(const std::string& in, std::string& out, States& new_state, User& parent);
      std::uint32_t onUsername(const std::string& in, std::string& base64Username);
      std::uint32_t onPassword(const std::string& in, std::string& base64Password);
      bool onLoginSuccess(const std::string& in, std::string& base64Username);
      std::uint32_t onCommand(const std::string& in, std::string& out, States& new_state);
  };

  class RCPT {
    public:
      RCPT() = default;
      ~RCPT() = default;

      void onEntry();
      void onExit();
      std::uint32_t onResponse(const std::string& in);
      std::uint32_t onResponse();
      std::uint32_t onResponse(const std::string& in, std::string& out, States& new_state, User& parent);
      std::uint32_t onCommand(const std::string& in, std::string& out, States& new_state);
  };

  class DATA {
    public:
      DATA() = default;
      ~DATA() = default;

      void onEntry();
      void onExit();
      std::uint32_t onResponse(const std::string& in);
      std::uint32_t onResponse();
      std::uint32_t onResponse(const std::string& in, std::string& out, States& new_state, User& parent);
      std::uint32_t onCommand(const std::string& in, std::string& out, States& new_state);
  };

  class BODY {
    public:
      BODY() = default;
      ~BODY() = default;

      void onEntry();
      void onExit();
      std::uint32_t onResponse(const std::string& in);
      std::uint32_t onResponse();
      std::uint32_t onResponse(const std::string& in, std::string& out, States& new_state, User& parent);
      std::uint32_t onCommand(const std::string& in, std::string& out, States& new_state);
  };
  class HELO {
    public:
      HELO() = default;
      ~HELO() = default;

      void onEntry();
      void onExit();
      std::uint32_t onResponse(const std::string& in);
      std::uint32_t onResponse();
      std::uint32_t onResponse(const std::string& in, std::string& out, States& new_state, User& parent);
      std::uint32_t onCommand(const std::string& in, std::string& out, States& new_state);
  };

  class QUIT {
    public:
      QUIT() = default;
      ~QUIT() = default;

      void onEntry();
      void onExit();
      std::uint32_t onResponse(const std::string& in);
      std::uint32_t onResponse();
      std::uint32_t onResponse(const std::string& in, std::string& out, States& new_state, User& parent);
      std::uint32_t onCommand(const std::string& in, std::string& out, States& new_state);
  };

  class RESET {
    public:
      RESET() = default;
      ~RESET() = default;

      void onEntry();
      void onExit();
      std::uint32_t onResponse(const std::string& in);
      std::uint32_t onResponse();
      std::uint32_t onResponse(const std::string& in, std::string& out, States& new_state, User& parent);
      std::uint32_t onCommand(const std::string& in, std::string& out, States& new_state);
  };

  class VRFY {
    public:
      VRFY() = default;
      ~VRFY() = default;

      void onEntry();
      void onExit();
      std::uint32_t onResponse(const std::string& in);
      std::uint32_t onResponse();
      std::uint32_t onResponse(const std::string& in, std::string& out, States& new_state, User& parent);
      std::uint32_t onCommand(const std::string& in, std::string& out, States& new_state);
  };

  class NOOP {
    public:
      NOOP() = default;
      ~NOOP() = default;

      void onEntry();
      void onExit();
      std::uint32_t onResponse(const std::string& in);
      std::uint32_t onResponse();
      std::uint32_t onResponse(const std::string& in, std::string& out, States& new_state, User& parent);
      std::uint32_t onCommand(const std::string& in, std::string& out, States& new_state);
  };

  class EXPN {
    public:
      EXPN() = default;
      ~EXPN() = default;

      void onEntry();
      void onExit();
      std::uint32_t onResponse(const std::string& in);
      std::uint32_t onResponse();
      std::uint32_t onResponse(const std::string& in, std::string& out, States& new_state, User& parent);
      std::uint32_t onCommand(const std::string& in, std::string& out, States& new_state);
  };

  class HELP {
    public:
      HELP() = default;
      ~HELP() = default;

      void onEntry();
      void onExit();
      std::uint32_t onResponse(const std::string& in);
      std::uint32_t onResponse();
      std::uint32_t onResponse(const std::string& in, std::string& out, States& new_state, User& parent);
      std::uint32_t onCommand(const std::string& in, std::string& out, States& new_state);
  };


  class Account {
    public:

      static Account& instance() {
        static Account inst;
        return(inst);
      }
      
      Account(const Account& acc) = delete;
      const Account& operator=(const Account&) = delete;

      Account& from_name(const std::string& in) {
        m_from_name = in;
        return(*this);
      }

      std::string from_name() const {
        return(m_from_name);
      }

      Account& from_email(const std::string& in) {
        m_from_email = in;
        return(*this);
      }

      std::string from_email() const {
        return(m_from_email);
      }

      Account& from_password(const std::string& in) {
        m_from_password = in;
        return(*this);
      }

      std::string from_password() const {
        return(m_from_password);
      }

      Account& to_email(const std::vector<std::string>& to_list) {
        m_to_email = to_list;
        return(*this);
      }

      std::vector<std::string> to_email() const {
        return(m_to_email);
      }

      Account& email_body(const std::string& in) {
        m_email_body = in;
        return(*this);
      }

      std::string email_body() const {
        return(m_email_body);
      }

      Account& email_subject(const std::string& in) {
        m_email_subject = in;
        return(*this);
      }

      std::string email_subject() const {
        return(m_email_subject);
      }

      Account& attachment(const std::vector<std::tuple<std::string, std::string>>& in) {
        m_attachment = in;
        return(*this);
      }

      std::vector<std::tuple<std::string, std::string>>& attachment() {
        return(m_attachment);
      }

    private:

      Account() {
         m_from_name.clear();
         m_from_email.clear();
         m_from_password.clear();
         m_to_email.clear();
         m_email_body.clear(); 
         m_email_subject.clear();
      }
            
      std::string m_from_name;
      std::string m_from_email;
      std::string m_from_password;
      std::vector<std::string> m_to_email;
      std::string m_email_subject;
      std::string m_email_body;
      std::vector<std::tuple<std::string, std::string>> m_attachment;
  };

  class User : public ACE_Event_Handler {
    public:
      User() {
        m_client = std::make_unique<Client>(25, "smtp.gmail.com", this);
        m_tls = std::make_unique<Tls>(this);
        
        m_response.clear();
      }

      ~User();
      std::int32_t startEmailTransaction();
      std::int32_t endEmailTransaction();
      
      void client(std::unique_ptr<SMTP::Client> smtpClient);
      const std::unique_ptr<SMTP::Client>& client() const;
      std::int32_t rx(const std::string& in);

      ACE_INT32 handle_timeout(const ACE_Time_Value& tv, const void* act) override;
      User& start_response_timeout_timer(ACE_Time_Value& tv);
      User& stop_response_timeout_timer();

      User& response_timer(long val) {
        m_response_timer = val; 
        return(*this);
      }

      long response_timer() const {
          return(m_response_timer);
      }

      auto& fsm() {
        return(m_fsm);
      }

      const std::unique_ptr<SMTP::Tls>& tls() const {
        return(m_tls);
      }

    private:
      /* Instance of SMTP User state is initialized with INIT */
      Transaction<States> m_fsm;
      /* Instance of SMTP TCP Client */
      std::unique_ptr<Client> m_client;
      /* Instance of SMTP TLS Client */
      std::unique_ptr<Tls> m_tls;
      /**/
      std::unordered_map<Response, std::string, hFn> m_response;
      long m_response_timer;
  };

}
#endif // EMAILSERVICE_HPP