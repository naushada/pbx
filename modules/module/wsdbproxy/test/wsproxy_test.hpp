#ifndef WSPROXY_TEST_HPP
#define WSPROXY_TEST_HPP

#include <gtest/gtest.h>
#include "wsdbproxy.hpp"

/// Test double for IWsDispatcher — captures last request, returns preset response.
class FakeWsDispatcher : public IWsDispatcher {
public:
  std::vector<uint8_t>  last_request;
  std::vector<uint8_t>  next_response;   // set before each test call

  std::vector<uint8_t> dispatch(const std::vector<uint8_t>& bson) override {
    last_request = bson;
    return next_response;
  }

  // Convenience: set an ok/sval response
  void will_respond_sval(int32_t reqid, const std::string& sval) {
    dbproto::DbResponse r;
    r.reqid = reqid;
    r.ok    = true;
    r.sval  = sval;
    next_response = dbproto::build_response(r);
  }

  void will_respond_ival(int32_t reqid, int32_t ival) {
    dbproto::DbResponse r;
    r.reqid = reqid;
    r.ok    = true;
    r.ival  = ival;
    next_response = dbproto::build_response(r);
  }

  void will_respond_bval(int32_t reqid, bool bval) {
    dbproto::DbResponse r;
    r.reqid = reqid;
    r.ok    = true;
    r.bval  = bval;
    next_response = dbproto::build_response(r);
  }

  void will_respond_data(int32_t reqid, const std::vector<uint8_t>& data) {
    dbproto::DbResponse r;
    r.reqid = reqid;
    r.ok    = true;
    r.data  = data;
    next_response = dbproto::build_response(r);
  }

  void will_respond_error(int32_t reqid, const std::string& msg) {
    next_response = dbproto::make_error_response(reqid, msg);
  }

  // Parse the last dispatched request BSON
  dbproto::DbRequest last_parsed() const {
    dbproto::DbRequest req;
    dbproto::parse_request(last_request, req);
    return req;
  }
};

#endif // WSPROXY_TEST_HPP
