#include "emailservice.hpp"
#include <fstream>
#include <sstream>
#include <regex>

std::uint32_t SMTP::parseSmtpCommand(const std::string& in, std::unordered_map<Response, std::string, hFn>& out)
{
    std::stringstream input(in);
    std::int32_t c;
    std::string elm("");
    out.clear();
    std::vector<std::string> arg;
    arg.clear();

    while((c = input.get()) != EOF) {
        switch(c) {
            case ' ':
            case '-':
            {
                arg.push_back(elm);
                auto cc = input.get();
                elm.clear();

                while(cc != ' ' && cc != EOF) {
                    if(cc == '\r') {
                        break;
                    } 
                    elm.push_back(cc);
                    cc = input.get();
                }

                if(cc == EOF) {
                    arg.push_back(elm);
                    out[Response(std::stoi(arg[0]), arg[1])] = "";
                    arg.clear();
                    elm.clear();
                    break;
                }

                /* get rid of ' ' or \n*/
                cc = input.get();
                if(cc == '\n') {
                    arg.push_back(elm);
                    out[Response(std::stoi(arg[0]), arg[1])] = "";
                    arg.clear();
                    elm.clear();
                    break;
                }

                /* get rid of ' ' */
                cc = input.get();
                arg.push_back(elm);
                elm.clear();

                while(cc != '\r' && cc != EOF) {
                    elm.push_back(cc);
                    cc = input.get();
                }

                /* get rid of '\n' */
                cc = input.get();
                arg.push_back(elm);
                out[SMTP::Response(std::stoi(arg[0]), arg[1])] = arg[2];
                arg.clear();
                elm.clear();
            }
            break;
            default:
            {
                elm.push_back(c);
            }
            break;
        }
    }
    return(0);
}

SMTP::Response SMTP::getSmtpStatusCode(const std::string& in)
{
    std::unordered_map<Response, std::string, hFn> out;
    out.clear();
    parseSmtpCommand(in, out);
    auto it = out.begin();
    return(it->first);
}

auto SMTP::find(const std::string& in, const std::string& what)
{
    std::unordered_map<Response, std::string, hFn> out;
    out.clear();
    parseSmtpCommand(in, out);
    auto it = out.begin();

    for(; it != out.end(); ++it)
    {
        auto ent = it->first;
        if(!what.compare(ent.m_statusCode)) {
            break;
        }
    }
    return(it);
}

std::uint32_t SMTP::getBase64(const std::string& in, std::string& b64Out)
{
    size_t out_len = 0;
    ACE_Byte* enc = ACE_Base64::encode((const ACE_Byte*)in.data(), in.length(), &out_len);
    b64Out.assign((char*)enc, out_len);
    free(enc);
    return(0);
}

void SMTP::display(const std::string& in)
{
    std::unordered_map<Response, std::string, hFn> commandList;
    parseSmtpCommand(in, commandList);
    for(const auto& elm: commandList) {
        ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l m_reply:%u m_statusCode:%s\n"), 
                   elm.first.m_reply, elm.first.m_statusCode.c_str()));        
    }
}

std::string SMTP::getContentType(const std::string& ext)
{
    if(!ext.compare("pdf")) {
        return("application/pdf");
    } else if(!ext.compare("docx") || !ext.compare("doc")) {
        return("application/msword");
    } else if(!ext.compare("zip")) {
        return("application/zip");
    } else if(!ext.compare("gzip")) {
        return("application/x-gzip");
    } else if(!ext.compare("xls") || !ext.compare("xlsx")) {
        return("application/vnd.ms-excel");
    } else if(!ext.compare("pps") || !ext.compare("ppt") || !ext.compare("pptx")) {
        return("application/vnd.ms-powerpoint");
    } else if(!ext.compare("jpeg") || !ext.compare("jpg") || !ext.compare("jpe")) {
        return("image/jpeg");
    } else if(!ext.compare("mpeg") || !ext.compare("mpg") || !ext.compare("mpe")) {
        return("video/mpeg");
    } else if(!ext.compare("gif")) {
        return("image/gif");
    } else if(!ext.compare("asc") || !ext.compare("diff") || !ext.compare("c") || !ext.compare("log") ||
              !ext.compare("patch") || !ext.compare("pot") || !ext.compare("text") || !ext.compare("txt")) {
        return("text/plain");
    }
    return("application/octet-stream");
}

void SMTP::GREETING::onEntry()
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::GREETING function:%s\n"), __PRETTY_FUNCTION__));
}

void SMTP::GREETING::onExit()
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::GREETING function:%s\n"), __PRETTY_FUNCTION__));
}

std::uint32_t SMTP::GREETING::onResponse(const std::string& in)
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::GREETING receive length:%d response:%s\n"), in.length(), in.c_str()));
    return(0);
}

std::uint32_t SMTP::GREETING::onResponse()
{
    return(0);
}

std::uint32_t SMTP::GREETING::onResponse(const std::string& in, std::string& out, States& new_state, User& parent)
{
    auto ent = getSmtpStatusCode(in);
    auto retStatus = 0;

    switch(ent.m_reply) {
        case SMTP::reply_code::REPLY_CODE_220_Service_ready:
        {
            display(in); 
            /* connection established successfully - send the next command */
            retStatus = onCommand(in, out, new_state);
            ACE_Time_Value to(1800,0);
            //parent.start_response_timeout_timer(to);
        }
        break;
        case SMTP::reply_code::REPLY_CODE_554_Transaction_has_failed:
        break;
        default:
            display(in);
        break;
    }
    return(retStatus);
}

/**
 * @brief This member method processes the incoming initial command from smtp server and sends EHLO - Extension Hello
 * 
 * @param in Greeting or Extension capabilities from smtp server
 * @param out response message to be sent to smtp server
 * @param new_state new state for processing of Extention cabalities 
 */
std::uint32_t SMTP::GREETING::onCommand(const std::string& in, std::string& out, States& new_state)
{
    std::stringstream ss("");
    ss << "EHLO gmail.com" << "\r\n";
    /// @brief modifiying out with response message to be sent to smtp server 
    out = ss.str();
    /// @brief moving to new state for processing of new command or request 
    new_state = HELO{};
    return(SMTP::status_code::GOTO_NEXT_STATE);
}

void SMTP::HELO::onEntry()
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::HELO function:%s\n"), __PRETTY_FUNCTION__));
}

void SMTP::HELO::onExit()
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::HELO function:%s\n"), __PRETTY_FUNCTION__));
}

std::uint32_t SMTP::HELO::onResponse(const std::string& in)
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::HELO receive length:%d response:%s\n"), 
               in.length(), in.c_str()));
    return(0);
}

std::uint32_t SMTP::HELO::onResponse()
{
    return(0);
}

std::uint32_t SMTP::HELO::onResponse(const std::string& in, std::string& out, States& new_state, User& parent)
{
    auto ent = getSmtpStatusCode(in);
    auto retStatus = 0;

    switch(ent.m_reply) {
        case SMTP::reply_code::REPLY_CODE_250_Request_mail_action_okay_completed:
            display(in);
            retStatus = onCommand(in, out, new_state);
        break;

        default:
            display(in);
        break;
    }
    return(retStatus);
}

/**
 * @brief This member method receives the response to TCP connect from SMTP server
 *        In this response SMTP server provides the capabilities of SMTP server, This 
 *        message is not encrypted.The SMTP client is checking to SMTP server to support
 *        TLS over TCP.
 * @param in Greeting or Extension capabilities from smtp server
 * @param out response message to be sent to smtp server
 * @param new_state new state for processing of Extention cabalities 
 */
std::uint32_t SMTP::HELO::onCommand(const std::string& in, std::string& out, States& new_state)
{
    std::unordered_map<SMTP::Response, std::string, SMTP::hFn> elm;
    elm.clear();
    parseSmtpCommand(in, elm);
    auto it = SMTP::find(in, "STARTTLS");

    if(it != elm.end()) {
        std::stringstream ss("");
        ss << "STARTTLS" << "\r\n";
        /// @brief modifiying out with response message to be sent to smtp server 
        out = ss.str();
        /// @brief moving to new state for processing of new command or request 
        new_state = MAIL{};
    } else {
        ACE_ERROR((LM_ERROR, ACE_TEXT("%D [mailservice:%t] %M %N:%l tls is not supported by SMTP server\n")));
        return(static_cast<std::uint32_t>(SMTP::status_code::TLS_NOT_SUPPORTED));
    }
    return(SMTP::status_code::GOTO_NEXT_STATE);
}

void SMTP::MAIL::onEntry()
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::MAIL function:%s\n"), __PRETTY_FUNCTION__));
    m_authStage = AUTH_INIT;
}

void SMTP::MAIL::onExit()
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::MAIL function:%s\n"), __PRETTY_FUNCTION__));
}

std::uint32_t SMTP::MAIL::onResponse(const std::string& in)
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::MAIL receive length:%d response:%s\n"),
               in.length(), in.c_str()));
    
    return(0);
}

std::uint32_t SMTP::MAIL::onResponse()
{
    return(0);
}

std::uint32_t SMTP::MAIL::onResponse(const std::string& in, std::string& out, States& new_state, User& parent)
{
    auto ent = getSmtpStatusCode(in);
    auto retStatus = 0;

    switch(ent.m_reply) {
        case SMTP::reply_code::REPLY_CODE_530_5_7_0_Authentication_needed:
        case SMTP::reply_code::REPLY_CODE_334_Server_challenge:
        case SMTP::reply_code::REPLY_CODE_250_Request_mail_action_okay_completed:
            display(in);
            retStatus = onCommand(in, out, new_state);
        break;
        case SMTP::reply_code::REPLY_CODE_535_5_7_8_Authentication_credentials_invalid:
        
        break;
        case SMTP::reply_code::REPLY_CODE_220_Service_ready:
            /* switch to TLS */
            ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::MAIL authnetication over tls started\n")));
            retStatus = onCommand(in, out, new_state);
        break;
        default:
            display(in);
            retStatus = onCommand(in, out, new_state);
        break;

    }
    return(retStatus);
}

std::uint32_t SMTP::MAIL::onCommand(const std::string& in, std::string& out, States& new_state)
{
    std::stringstream ss("");

    if(AUTH_INIT == m_authStage) {
        std::string result("");
        ss << "AUTH LOGIN" << "\r\n";
        /// @brief modifiying out with response message to be sent to smtp server 
        out = ss.str();
        m_authStage = AUTH_USRNAME;
        /// @brief remain in same state
        return(SMTP::status_code::REMAIN_IN_SAME_STATE);

    } else if(AUTH_USRNAME == m_authStage) {
        if(!onUsername(in, out)) {
            m_authStage = AUTH_PASSWORD;
        }
        /// @brief remain in same state
        return(SMTP::status_code::REMAIN_IN_SAME_STATE);

    } else if(AUTH_PASSWORD == m_authStage) {
        if(!onPassword(in, out)) { 
            m_authStage = AUTH_SUCCESS;
        }
        /// @brief remain in same state
        return(SMTP::status_code::REMAIN_IN_SAME_STATE);

    } else if(AUTH_SUCCESS == m_authStage) {
        if(!onLoginSuccess(in, out)) {
          ss << "RSET" << "\r\n";
          out = ss.str();
          m_authStage = AUTH_INIT;
          new_state = RESET{};
          return(SMTP::status_code::INCORRECT_LOGIN_CREDENTIALS);
        } else {
            new_state = RCPT{};
            return(SMTP::status_code::GOTO_NEXT_STATE);
        }
    } 
    return(SMTP::status_code::ERROR_END);
}

std::uint32_t SMTP::MAIL::onUsername(const std::string& in, std::string& base64Username)
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::MAIL function:%s\n"),__PRETTY_FUNCTION__));
    size_t out_len = 0;
    auto ent = SMTP::getSmtpStatusCode(in);
    
    if((SMTP::reply_code::REPLY_CODE_334_Server_challenge == ent.m_reply) && !ent.m_statusCode.empty()) {
        const ACE_Byte* data = (ACE_Byte *)(ent.m_statusCode.data());
        ACE_Byte* plain = ACE_Base64::decode(data, &out_len);

        if(plain) {
            std::string usrName((char *)plain, out_len);
            if(!usrName.compare("Username:")) {
                std::string nm = Account::instance().from_email();
                ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l username:%s\n"), Account::instance().from_email().c_str()));
                std::string b64_;
                auto len = SMTP::getBase64(nm, b64_);
                std::stringstream ss("");
                ss << b64_;
                base64Username = ss.str();
                return(SMTP::status_code::OK);
            }
            ACE_ERROR((LM_ERROR, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::MAIL challenge for username:%s\n"),usrName.c_str()));
            return(SMTP::status_code::CHALLENGE_FOR_USERNAME_FAILED);
        }
    }
    return(SMTP::status_code::BASE64_DECODING_FAILED);
}

std::uint32_t SMTP::MAIL::onPassword(const std::string& in, std::string& base64Username)
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::MAIL function:%s\n"),__PRETTY_FUNCTION__));
    size_t out_len = 0;
    auto ent = SMTP::getSmtpStatusCode(in);
    if(SMTP::reply_code::REPLY_CODE_334_Server_challenge == ent.m_reply && 
       !ent.m_statusCode.empty()) {

        const ACE_Byte* data = (ACE_Byte *)(ent.m_statusCode.data());
        ACE_Byte* plain = ACE_Base64::decode(data, &out_len);
        if(plain) {
            std::string pwd((char *)plain, out_len);
            if(!pwd.compare("Password:")) {
                std::string nm = Account::instance().from_password();
                std::string b64_;
                auto len = SMTP::getBase64(nm,b64_);
                std::stringstream ss("");
                ss << b64_;
                base64Username = ss.str();
                return(SMTP::status_code::OK);
            }
            ACE_ERROR((LM_ERROR, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::MAIL challenge for username:%s\n"),pwd.c_str()));
            return(SMTP::status_code::CHALLENGE_FOR_PASSWORD_FAILED);
        }
    }
    return(SMTP::status_code::BASE64_DECODING_FAILED);
}

bool SMTP::MAIL::onLoginSuccess(const std::string& in, std::string& out)
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::MAIL function:%s\n"),__PRETTY_FUNCTION__));

    auto ent = SMTP::getSmtpStatusCode(in);
    if(SMTP::reply_code::REPLY_CODE_235_2_7_0_Authentication_succeeded == ent.m_reply && 
       !ent.m_statusCode.compare("2.7.0")) {
        ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::MAIL authentication over tls is sucessful\n")));

        std::stringstream ss("");
        ss << "MAIL FROM: <" << Account::instance().from_email() << ">\r\n";
        /// @brief modifiying out with response message to be sent to smtp server 
        out = ss.str();
        return(true);
    }
    return(false);
}

void SMTP::DATA::onEntry()
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::DATA function:%s\n"), __PRETTY_FUNCTION__));
}

void SMTP::DATA::onExit()
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::DATA function:%s\n"), __PRETTY_FUNCTION__));
}

std::uint32_t SMTP::DATA::onResponse(const std::string& in)
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::DATA receive length:%d response:%s\n"), in.length(), in.c_str()));
    return(0);
}

std::uint32_t SMTP::DATA::onResponse()
{
    return(0);
}

std::uint32_t SMTP::DATA::onResponse(const std::string& in, std::string& out, States& new_state, User& parent)
{
    auto ent = getSmtpStatusCode(in);
    auto retStatus = 0;
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::DATA m_reply:%u m_statusCode:%s\n"), 
              ent.m_reply, ent.m_statusCode.c_str()));

    switch(ent.m_reply) {
        case SMTP::reply_code::REPLY_CODE_235_2_7_0_Authentication_succeeded:
            display(in);
            retStatus = onCommand(in, out, new_state);
        break;
        case SMTP::reply_code::REPLY_CODE_535_5_7_8_Authentication_credentials_invalid:
        
        break;
        case SMTP::reply_code::REPLY_CODE_220_Service_ready:
            ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::MAIL authnetication over tls started\n")));
            retStatus = onCommand(in, out, new_state);
        break;
        default:
            display(in);
            retStatus = onCommand(in, out, new_state);
        break;

    }
    return(retStatus);
}
std::uint32_t SMTP::DATA::onCommand(const std::string& in, std::string& out, States& new_state)
{

    std::stringstream ss("");
    ss << "DATA" << "\r\n";
    /// @brief modifiying out with response message to be sent to smtp server 
    out = ss.str();
 
    new_state = BODY{};
    return(SMTP::status_code::GOTO_NEXT_STATE);
}

void SMTP::BODY::onEntry()
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::BODY function:%s\n"), __PRETTY_FUNCTION__));
}

void SMTP::BODY::onExit()
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::BODY function:%s\n"), __PRETTY_FUNCTION__));
}

std::uint32_t SMTP::BODY::onResponse(const std::string& in)
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::BODY receive length:%d response:%s\n"), in.length(), in.c_str()));
    return(0);
}

std::uint32_t SMTP::BODY::onResponse()
{
    return(0);
}
std::uint32_t SMTP::BODY::onResponse(const std::string& in, std::string& out, States& new_state, User& parent)
{
    auto ent = getSmtpStatusCode(in);
    auto retStatus = 0;
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::BODY m_reply:%u m_statusCode:%s\n"), 
              ent.m_reply, ent.m_statusCode.c_str()));

    switch(ent.m_reply) {
        case SMTP::reply_code::REPLY_CODE_250_Request_mail_action_okay_completed:
            display(in);
            retStatus = onCommand(in, out, new_state);
        break;
        case SMTP::reply_code::REPLY_CODE_535_5_7_8_Authentication_credentials_invalid:
        
        break;
        case SMTP::reply_code::REPLY_CODE_220_Service_ready:
            retStatus = onCommand(in, out, new_state);
        break;
        default:
            display(in);
            retStatus = onCommand(in, out, new_state);
        break;

    }
    return(retStatus);
}

namespace {

std::string build_mime_message()
{
    auto& acc = SMTP::Account::instance();
    std::time_t now = std::time(nullptr);
    std::stringstream ss;

    ss << "MIME-Version: 1.0\r\n"
       << "From: " << acc.from_name() << " <" << acc.from_email() << ">\r\n"
       << "To: ";
    for (const auto& addr : acc.to_email())
        ss << addr << ";";
    ss << "\r\n"
       << "Subject: " << acc.email_subject() << "\r\n"
       << "Date: " << std::asctime(std::localtime(&now));

    const auto& list = acc.attachment();
    if (list.empty()) {
        ss << "Content-type: text/plain; charset=us-ascii\r\n\r\n"
           << acc.email_body() << "\r\n";
    } else {
        ss << "Content-Type: multipart/mixed; boundary=cordoba\r\n"
           << "--cordoba\r\n"
           << "Content-type: text/plain; charset=us-ascii\r\n\r\n"
           << acc.email_body() << "\r\n";

        for (const auto& [fname, fcontent] : list) {
            if (fname.empty()) {
                ACE_ERROR((LM_ERROR, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::BODY file_name:empty\n")));
                continue;
            }
            std::size_t dot = fname.find('.');
            std::string type = SMTP::getContentType(dot != std::string::npos ? fname.substr(dot + 1) : "");

            std::ifstream ifs(fname);
            if (!ifs.is_open()) {
                ACE_ERROR((LM_ERROR, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::BODY file_name:%s not found, using inline content\n"), fname.c_str()));
                if (!fcontent.empty()) {
                    ss << "--cordoba\r\n"
                       << "Content-Transfer-Encoding: base64\r\n"
                       << "Content-Type: " << type << "\r\n"
                       << "Content-Disposition: attachment; filename=" << fname
                       << " ;size-parm=" << fcontent.length() << "\r\n\r\n"
                       << fcontent << "\r\n";
                }
            } else {
                std::stringstream contents;
                contents << ifs.rdbuf();
                std::string b64;
                SMTP::getBase64(contents.str(), b64);
                ss << "--cordoba\r\n"
                   << "Content-Transfer-Encoding: base64\r\n"
                   << "Content-Type: " << type << "\r\n"
                   << "Content-Disposition: attachment; filename=" << fname
                   << " ;size-parm=" << contents.str().length() << "\r\n\r\n"
                   << b64 << "\r\n";
            }
        }
        ss << "--cordoba--\r\n";
    }
    ss << "\r\n.\r\n";
    return ss.str();
}

} // namespace

std::uint32_t SMTP::BODY::onCommand(const std::string& in, std::string& out, States& new_state)
{
    out = build_mime_message();
    new_state = QUIT{};
    return(SMTP::status_code::GOTO_NEXT_STATE);
}

void SMTP::RCPT::onEntry()
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::RCPT function:%s\n"), __PRETTY_FUNCTION__));
}

void SMTP::RCPT::onExit()
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::RCPT function:%s\n"), __PRETTY_FUNCTION__));
}

std::uint32_t SMTP::RCPT::onResponse(const std::string& in)
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::RCPT receive length:%d response:%s\n"), in.length(), in.c_str()));
    return(0);
}

std::uint32_t SMTP::RCPT::onResponse()
{
    return(0);
}

std::uint32_t SMTP::RCPT::onResponse(const std::string& in, std::string& out, States& new_state, User& parent)
{
    auto ent = getSmtpStatusCode(in);
    auto retStatus = 0;
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::RCPT m_reply:%u m_statusCode:%s\n"), 
              ent.m_reply, ent.m_statusCode.c_str()));

    switch(ent.m_reply) {
        case SMTP::reply_code::REPLY_CODE_250_Request_mail_action_okay_completed:
        {
            if(!ent.m_statusCode.compare("2.1.0")) {
                display(in);
                retStatus = onCommand(in, out, new_state);
            }
        }
        break;
        case SMTP::reply_code::REPLY_CODE_535_5_7_8_Authentication_credentials_invalid:
        
        break;
        case SMTP::reply_code::REPLY_CODE_220_Service_ready:
            retStatus = onCommand(in, out, new_state);
        break;
        default:
            display(in);
            retStatus = onCommand(in, out, new_state);
        break;

    }
    return(retStatus);
}

std::uint32_t SMTP::RCPT::onCommand(const std::string& in, std::string& out, States& new_state)
{

    std::stringstream ss("");
    for(const auto& elm : SMTP::Account::instance().to_email()) {
        ss << "RCPT TO: <" << elm << ">\r\n";
    }
    /// @brief modifiying out with response message to be sent to smtp server 
    out = ss.str();
    new_state = DATA{};
    return(SMTP::status_code::GOTO_NEXT_STATE);
}

void SMTP::QUIT::onEntry()
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::QUIT function:%s\n"), __PRETTY_FUNCTION__));
}

void SMTP::QUIT::onExit()
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::QUIT function:%s\n"), __PRETTY_FUNCTION__));
}

std::uint32_t SMTP::QUIT::onResponse(const std::string& in)
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST:MAIL receive length:%d response:%s\n"), in.length(), in.c_str()));
    
    return(0);
}

std::uint32_t SMTP::QUIT::onResponse()
{
    return(0);
}

std::uint32_t SMTP::QUIT::onResponse(const std::string& in, std::string& out, States& new_state, User& parent)
{
    auto ent = getSmtpStatusCode(in);
    auto retStatus = 0;

    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::QUIT m_reply:%u m_statusCode:%s\n"), 
              ent.m_reply, ent.m_statusCode.c_str()));

    switch(ent.m_reply) {
        case SMTP::reply_code::REPLY_CODE_250_Request_mail_action_okay_completed:
            display(in);
            if(!ent.m_statusCode.compare("2.0.0")) {
                retStatus = onCommand(in, out, new_state);
            }
        break;
        case SMTP::reply_code::REPLY_CODE_221_Service_closing_transmission_channel:
            if(!ent.m_statusCode.compare("2.0.0")) {
                ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::QUIT Closing the e-mailsession\n")));
                return(SMTP::status_code::REMAIN_IN_SAME_STATE);
            }
        break;
        case SMTP::reply_code::REPLY_CODE_220_Service_ready:
            retStatus = onCommand(in, out, new_state);
        break;
        default:
            display(in);
            retStatus = onCommand(in, out, new_state);
        break;

    }
    return(retStatus);
}

std::uint32_t SMTP::QUIT::onCommand(const std::string& in, std::string& out, States& new_state)
{

    std::stringstream ss("");
    ss << "QUIT" << "\r\n";
    /// @brief modifiying out with response message to be sent to smtp server 
    out = ss.str();

    return(SMTP::status_code::REMAIN_IN_SAME_STATE);

}

void SMTP::RESET::onEntry()
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::RESET function:%s\n"), __PRETTY_FUNCTION__));
}

void SMTP::RESET::onExit()
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::RESET function:%s\n"), __PRETTY_FUNCTION__));
}

std::uint32_t SMTP::RESET::onResponse(const std::string& in)
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST:MAIL receive length:%d response:%s\n"), in.length(), in.c_str()));
    
    return(0);
}

std::uint32_t SMTP::RESET::onResponse()
{
    return(0);
}

std::uint32_t SMTP::RESET::onResponse(const std::string& in, std::string& out, States& new_state, User& parent)
{
    return 0;
}

std::uint32_t SMTP::RESET::onCommand(const std::string& in, std::string& out, States& new_state)
{
    return 0;
}

void SMTP::VRFY::onEntry()
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::VRFY function:%s\n"), __PRETTY_FUNCTION__));
}

void SMTP::VRFY::onExit()
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::VRFY function:%s\n"), __PRETTY_FUNCTION__));
}

std::uint32_t SMTP::VRFY::onResponse(const std::string& in)
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::MAIL receive length:%d response:%s\n"), in.length(), in.c_str()));
    
    return(0);
}

std::uint32_t SMTP::VRFY::onResponse()
{
    return(0);
}

std::uint32_t SMTP::VRFY::onResponse(const std::string& in, std::string& out, States& new_state, User& parent)
{
    return 0;
}

std::uint32_t SMTP::VRFY::onCommand(const std::string& in, std::string& out, States& new_state)
{
    return 0;
}


void SMTP::NOOP::onEntry()
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::NOOP function:%s\n"), __PRETTY_FUNCTION__));
}

void SMTP::NOOP::onExit()
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::NOOP function:%s\n"), __PRETTY_FUNCTION__));
}

std::uint32_t SMTP::NOOP::onResponse(const std::string& in)
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::MAIL receive length:%d response:%s\n"), in.length(), in.c_str()));
    
    return(0);
}

std::uint32_t SMTP::NOOP::onResponse()
{
    return(0);
}

std::uint32_t SMTP::NOOP::onResponse(const std::string& in, std::string& out, States& new_state, User& parent)
{
    return 0;
}
std::uint32_t SMTP::NOOP::onCommand(const std::string& in, std::string& out, States& new_state)
{
    return 0;
}

void SMTP::EXPN::onEntry()
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::EXPN function:%s\n"), __PRETTY_FUNCTION__));
}

void SMTP::EXPN::onExit()
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::EXPN function:%s\n"), __PRETTY_FUNCTION__));
}

std::uint32_t SMTP::EXPN::onResponse(const std::string& in)
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::MAIL receive length:%d response:%s\n"), in.length(), in.c_str()));
    
    return(0);
}

std::uint32_t SMTP::EXPN::onResponse()
{
    return(0);
}

std::uint32_t SMTP::EXPN::onResponse(const std::string& in, std::string& out, States& new_state, User& parent)
{
    return 0;
}
std::uint32_t SMTP::EXPN::onCommand(const std::string& in, std::string& out, States& new_state)
{
    return 0;
}

void SMTP::HELP::onEntry()
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::HELP function:%s\n"), __PRETTY_FUNCTION__));
}

void SMTP::HELP::onExit()
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::HELP function:%s\n"), __PRETTY_FUNCTION__));
}

std::uint32_t SMTP::HELP::onResponse(const std::string& in)
{
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [mailservice:%t] %M %N:%l ST::MAIL receive length:%d response:%s\n"), in.length(), in.c_str()));
    
    return(0);
}

std::uint32_t SMTP::HELP::onResponse()
{
    return(0);
}

std::uint32_t SMTP::HELP::onResponse(const std::string& in, std::string& out, States& new_state, User& parent)
{
    auto ent = SMTP::getSmtpStatusCode(in);
    switch(ent.m_reply) {
        case SMTP::reply_code::REPLY_CODE_250_Request_mail_action_okay_completed:
        break;
        default:
        break;
    }
    return 0;
}

std::uint32_t SMTP::HELP::onCommand(const std::string& in, std::string& out, States& new_state)
{
    std::stringstream ss("");
    ss << "AUTH LOGIN" << "\r\n";
    /// @brief modifiying out with response message to be sent to smtp server 
    out = ss.str();
    /// @brief moving to new state for processing of new command or request 
    new_state = MAIL{};
    return(0);
}

