#include "message_parser.hpp"
#include <algorithm>
#include <cctype>
#include <sstream>

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

std::string pct_decode(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+') {
      out.push_back(' ');
    } else if (s[i] == '%' && i + 2 < s.size()) {
      char hex[3] = {s[i + 1], s[i + 2], '\0'};
      out.push_back(static_cast<char>(std::stoi(hex, nullptr, 16)));
      i += 2;
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

std::string MessageParser::extract_header(const std::string &in) {
  const std::string delim("\r\n\r\n");
  auto offset = in.find(delim);
  if (offset != std::string::npos)
    return in.substr(0, offset + delim.size());
  return in;
}

void MessageParser::add_element(std::string key, std::string value) {
  m_tokenMap.emplace(to_lower(std::move(key)), std::move(value));
}

std::string MessageParser::get_element(const std::string &key) const {
  auto it = m_tokenMap.find(to_lower(key));
  return (it != m_tokenMap.end()) ? it->second : std::string{};
}

void MessageParser::parse_mime_header(const std::string &in) {
  std::istringstream input(in);
  std::string line;

  // Skip the first line (request line or status line).
  std::getline(input, line);

  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty())
      break;

    auto colon = line.find(':');
    if (colon == std::string::npos)
      continue;

    std::string param = line.substr(0, colon);
    std::string value =
        (colon + 2 <= line.size()) ? line.substr(colon + 2) : std::string{};
    add_element(std::move(param), std::move(value));
  }
}
