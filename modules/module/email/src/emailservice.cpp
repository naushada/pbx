#include "emailservice.hpp"
#include <cassert>

SMTP::Tls::Tls(User* parent) : m_ssl(nullptr), m_sslCtx(nullptr), m_user(parent), m_isTlsUP(false)
{
}

SMTP::Tls::~Tls()
{
    SSL_free(m_ssl);
    SSL_CTX_free(m_sslCtx);
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [tlsservice:%t] %M %N:%l TLS service is freed\n")));
}

void SMTP::Tls::init()
{
    const SSL_METHOD *method;
    /* Load cryptos, et.al. */
    OpenSSL_add_all_algorithms();	
    /* Bring in and register error messages */	
    SSL_load_error_strings();			
    /* Create new client-method instance */
    method = SSLv23_client_method();		
    /* Create new context */
    m_sslCtx = SSL_CTX_new(method);			
    if(m_sslCtx == NULL) {
        ACE_ERROR((LM_ERROR, ACE_TEXT("%D [tlsservice:%t] %M %N:%l ssl context creation is failed aborting it.\n")));
        ERR_print_errors_fp(stderr);
        abort();
    }

    /* ---------------------------------------------------------- *
     * Disabling SSLv2 will leave v3 and TSLv1 for negotiation    *
     * ---------------------------------------------------------- */
    SSL_CTX_set_options(m_sslCtx, SSL_OP_NO_SSLv2);
}

std::int32_t SMTP::Tls::start(std::int32_t tcp_handle)
{
    std::int32_t rc = -1;

    init();
    /*create new SSL connection state*/
    m_ssl = SSL_new(m_sslCtx);
    /*continue as long as m_ssl is not NULL*/
    assert(m_ssl != NULL);

    /*attach the tcp socket descriptor to SSL*/
    rc = SSL_set_fd(m_ssl, tcp_handle);
    assert(rc == 1);
  	
    /*Initiate ClientHello Message to TLS Server*/
    if((rc = SSL_connect(m_ssl)) != 1) {
        ACE_ERROR((LM_ERROR, ACE_TEXT("%D [tlsservice:%t] %M %N:%l SSL_connect_error:%d\n"), SSL_get_error(m_ssl, rc)));
        /*TLS/SSL handshake is not successfullu*/
        SSL_free(m_ssl);
        SSL_CTX_free(m_sslCtx);
        isTlsUP(false);
    }

    isTlsUP(true);
    return(rc);
}

std::int32_t SMTP::Tls::read(std::string& plain_buffer)
{
    std::int32_t rc = -1;
    std::array<char, 2048> rd;
    rd.fill(0);
    rc = SSL_read(m_ssl, rd.data(), rd.max_size());
    if(rc <= 0) {
        ACE_ERROR((LM_ERROR, ACE_TEXT("%D [tlsservice:%t] %M %N:%l SSL_read is failed %d\n"), SSL_get_error(m_ssl, rc)));
    } else {
      plain_buffer = std::string(rd.data(), rc);
    }
    return(rc);
}

std::int32_t SMTP::Tls::write(const std::string& plain_buffer)
{
    std::int32_t rc = -1;
    std::int32_t offset = 0;
    std::int32_t len = static_cast<std::int32_t>(plain_buffer.length());
    
    do {
        rc = SSL_write(m_ssl, (plain_buffer.data() + offset), (len - offset));
        if(rc <= 0) {
            ACE_ERROR((LM_ERROR, ACE_TEXT("%D [tlsservice:%t] %M %N:%l SSL_write is rc%d err:%d\n"), rc, SSL_get_error(m_ssl, rc)));
        } else {
            offset += rc;
            ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [tlsservice:%t] %M %N:%l SSL_write is length:%d\n"), rc));
        }
    }while(offset != len);
    return(rc);
}

std::int32_t SMTP::Tls::peek(std::string& plain_buffer)
{
    std::int32_t rc = -1;
    rc = SSL_peek(m_ssl, plain_buffer.data(), 512);
    return(rc);
}

void SMTP::Tls::close()
{

}

SMTP::Client::Client(ACE_UINT16 port, std::string addr, User* user)
{
  m_user = user;
  m_mb = nullptr;
  m_mailServiceAvailable = false;
  m_smtpServerAddress.set(port, addr.c_str());
  m_semaphore = std::make_unique<ACE_Semaphore>();
}

SMTP::Client::~Client()
{
  m_semaphore.reset(nullptr);
  
}

int SMTP::Client::svc(void) {
  m_semaphore->release();
  ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l semaphore is released and going into main-loop\n")));
  main();
  ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l active object is stopped now\n")));
  return(0);
}

int SMTP::Client::open(void *args) {
  ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l active object is spawned\n")));
  activate();
  return(0);
}

int SMTP::Client::close(u_long flags) {
  ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l active object is closed now\n")));
  return(0);
}

ACE_INT32 SMTP::Client::handle_timeout(const ACE_Time_Value &tv, const void *act)
{
  return(0);
}

ACE_INT32 SMTP::Client::handle_input(ACE_HANDLE handle)
{
  size_t ret = -1;
  
  if(!user().tls()->isTlsUP()) {
      std::array<std::uint8_t, 2048> in;
      in.fill(0);
      ret = m_stream.recv((void *)in.data(), in.max_size());
      std::string ss((char *)in.data(), ret);
      ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l received over plain tcp length:%d response:%s\n"), ret, ss.c_str()));
      user().rx(ss);

  } else {
    std::string ss;
    ss.clear();
    ret = user().tls()->read(ss);
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l received over tls length:%d response:%s\n"), ret, ss.c_str()));
    if(ret) {
        user().rx(ss);
    } else {
        // closing the connection now.
        m_mailServiceAvailable = false;
        m_semaphore->acquire();
        stop();
        return(0);
    }
  }
  
  /// @return upon success returns zero meaning middleware will continue the reactor loop else it breaks the loop
  return(0);
}

ACE_INT32 SMTP::Client::handle_signal(int signum, siginfo_t *s, ucontext_t *u)
{
  ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [Master:%t] %M %N:%l Signal Number %d and its name %S is received for emailservice\n"), signum, signum));
  ACE_Reactor::instance()->remove_handler(m_stream.get_handle(), 
                                                         ACE_Event_Handler::ACCEPT_MASK | 
                                                         ACE_Event_Handler::TIMER_MASK | 
                                                         ACE_Event_Handler::SIGNAL_MASK |
                                                         ACE_Event_Handler::READ_MASK);
  return(0);

}

std::int32_t SMTP::Client::tx(const std::string& in)
{
  std::int32_t txLen = -1;

  if(m_mailServiceAvailable) {

    txLen = m_stream.send_n(in.c_str(), in.length());

    if(txLen < 0) {
      ACE_ERROR((LM_ERROR, ACE_TEXT("%D [mailservice:%t] %M %N:%l send_n to %s and port %u is failed for length %u\n"), 
        m_smtpServerAddress.get_host_name(), m_smtpServerAddress.get_port_number(), in.length()));
    }

    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l successfully sent of length %d on handle %u\n"), in.length(), m_stream.get_handle()));
  }
  return(txLen);
}

/**
 * @brief This member function is invoked when remove_handler or any of handle_xxx returns -1
 * 
 * @param fd the handle to be closed
 * @param mask mask of this handle
 * @return ACE_INT32 always 0
 */
ACE_INT32 SMTP::Client::handle_close(ACE_HANDLE fd, ACE_Reactor_Mask mask)
{
  m_mailServiceAvailable = false;
  close(fd);
  return(0);
}

/**
 * @brief This member method is invoked when we invoke register_handle
 * 
 * @return ACE_HANDLE returns the registered handle
 */
ACE_HANDLE SMTP::Client::get_handle() const
{
  ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l fn:%s handle:%u\n"), __PRETTY_FUNCTION__, m_stream.get_handle()));
  return(m_stream.get_handle());
}

/**
 * @brief This function starts the SMTP client by establishing secure connection
 * 
 */
void SMTP::Client::start()
{
  ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l entry_fn:%s \n"), __PRETTY_FUNCTION__));
  do {
    if(m_connection.connect(m_stream, m_smtpServerAddress)) {
      ACE_ERROR((LM_ERROR, ACE_TEXT("%D [mailservice:%t] %M %N:%l connect to host-name:%s and port-number:%u is failed\n"), 
           m_smtpServerAddress.get_host_name(), m_smtpServerAddress.get_port_number()));
      m_mailServiceAvailable = false;
      break;
    }

    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l connect host-name:%s, ip-address:%s and port:%u is success\n"), 
           m_smtpServerAddress.get_host_name(),m_smtpServerAddress.get_host_addr(), m_smtpServerAddress.get_port_number()));

    /* Feed this new handle to event Handler for read/write operation. */
    ACE_Reactor::instance()->register_handler(m_stream.get_handle(), this, ACE_Event_Handler::READ_MASK |
                                                                           ACE_Event_Handler::TIMER_MASK |
                                                                           ACE_Event_Handler::SIGNAL_MASK);

    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [emailservice:%t] %M %N:%l The emailservice is connected at handle:%u\n"), m_stream.get_handle()));
    m_mailServiceAvailable = true;

    /* subscribe for signal */
    ss.empty_set();
    ss.sig_add(SIGINT);
    ss.sig_add(SIGTERM);
    ACE_Reactor::instance()->register_handler(&ss, this);
    // spawn an active object for waiting on reactor
    open();

  }while(0);

}

void SMTP::Client::main() 
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l entry_fn:%s \n"), __PRETTY_FUNCTION__));
    ACE_Time_Value to(1,0);
    while(m_mailServiceAvailable) ACE_Reactor::instance()->handle_events(to);

    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l active object cease to exists\n")));
    m_semaphore->release();
}

void SMTP::Client::stop()
{
    ACE_Reactor::instance()->remove_handler(m_stream.get_handle(), ACE_Event_Handler::ACCEPT_MASK |
                                                                   ACE_Event_Handler::TIMER_MASK |
                                                                   ACE_Event_Handler::SIGNAL_MASK |
                                                                   ACE_Event_Handler::READ_MASK);
    m_mailServiceAvailable = false;
}

SMTP::User::~User()
{
  ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l entry_fn:%s \n"), __PRETTY_FUNCTION__));
  m_client.release();
  m_tls.release();
  ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [emailservice:%t] %M %N:%l dtor of User\n")));
}

/**
 * @brief This is the entry function for e-mail client.
 * 
 * @return std::int32_t 
 */
std::int32_t SMTP::User::startEmailTransaction()
{
  ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l entry_fn:%s \n"), __PRETTY_FUNCTION__));
  std::int32_t ret = 0;
  std::string req;
  client()->start();
  ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [emailservice:%t] %M %N:%l acquiring semaphore\n")));
  client()->m_semaphore->acquire();
  ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [emailservice:%t] %M %N:%l semaphore is released\n")));
  /* upon connection establishment to smtp server, smtp server replies with greeting */
  fsm().set_state(GREETING());
  
  /*block the control here */
  while(client()->m_mailServiceAvailable) ;
  return(ret);
}

std::int32_t SMTP::User::endEmailTransaction()
{
  std::int32_t ret = 0;
  client()->m_mailServiceAvailable = false;
  client()->m_semaphore->acquire();
  //m_client.reset(nullptr);
  m_client.release();

  return(ret);
}

/**
 * @brief This member method is invoked by SMTP client upon receipt of response from SMTP server
 * 
 * @param out response string from SMTP server
 * @return std::int32_t returns > 0 upon success else less than 0
 */
std::int32_t SMTP::User::rx(const std::string& in)
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l entry_fn:%s \n"), __PRETTY_FUNCTION__));

    auto ent = SMTP::getSmtpStatusCode(in);
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l m_status:%u \n"), ent.m_reply));

    // Dispatch FSM event, send the resulting command, and transition state.
    // end_on_error: call endEmailTransaction() when FSM returns an error code.
    auto dispatch = [&](bool end_on_error) {
        std::string cmd;
        SMTP::States new_state;
        auto result = fsm().onRx(in, cmd, new_state, *this);
        if (cmd.length()) {
            size_t len = -1;
            if (tls()->isTlsUP()) {
                len = m_tls->write(cmd);
                ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l sent over tls length:%d command:%s\n"), len, cmd.c_str()));
            } else {
                len = client()->tx(cmd);
                ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l sent over plain tcp length:%d command:%s\n"), len, cmd.c_str()));
            }
        }
        switch (result) {
            case SMTP::status_code::GOTO_NEXT_STATE:
                fsm().set_state(new_state);
                break;
            case SMTP::status_code::REMAIN_IN_SAME_STATE:
                break;
            default:
                if (end_on_error) endEmailTransaction();
                break;
        }
    };

    if (ent.m_reply == SMTP::REPLY_CODE_220_Service_ready) {
        if (!ent.m_statusCode.compare("2.0.0"))
            m_tls->start(client()->get_handle());
        dispatch(true);
    } else {
        dispatch(false);
    }

    return(0);
}

void SMTP::User::client(std::unique_ptr<SMTP::Client> smtpClient)
{
  m_client = std::move(smtpClient);
}

const std::unique_ptr<SMTP::Client>& SMTP::User::client() const
{
  return(m_client);
}

ACE_INT32 SMTP::User::handle_timeout(const ACE_Time_Value& tv, const void* act)
{
    return(0);
}

SMTP::User& SMTP::User::start_response_timeout_timer(ACE_Time_Value &to)
{
    m_response_timer = ACE_Reactor::instance()->schedule_timer(this, (const void *)nullptr, to);
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [emailservice:%t] %M %N:%l Command response timeout timer is started successfully\n")));
    return(*this);
}

SMTP::User& SMTP::User::stop_response_timeout_timer()
{
    if(ACE_Reactor::instance()->cancel_timer(m_response_timer)) {
        ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [emailservice:%t] %M %N:%l Command response timeout timer is stopped successfully\n")));
    }
    return(*this);
}

