#ifndef DBPROTO_TEST_HPP
#define DBPROTO_TEST_HPP

#include <gtest/gtest.h>
#include "dbproto.hpp"

// Helper: build a minimal BSON document from a JSON string using bsoncxx.
// Used to create doc/doc2 payloads for build_request() calls.
#include <bsoncxx/json.hpp>
inline std::vector<std::uint8_t> json_to_bson_bytes(const std::string& json) {
    auto doc = bsoncxx::from_json(json);
    return {doc.view().data(), doc.view().data() + doc.view().length()};
}

// Helper: build an error-free response BSON for a given reqid + sval.
inline std::vector<std::uint8_t> make_ok_sval_response(std::int32_t reqid,
                                                        const std::string& sval) {
    dbproto::DbResponse r;
    r.reqid = reqid;
    r.ok    = true;
    r.sval  = sval;
    return dbproto::build_response(r);
}

inline std::vector<std::uint8_t> make_ok_ival_response(std::int32_t reqid,
                                                        std::int32_t ival) {
    dbproto::DbResponse r;
    r.reqid = reqid;
    r.ok    = true;
    r.ival  = ival;
    return dbproto::build_response(r);
}

inline std::vector<std::uint8_t> make_ok_bval_response(std::int32_t reqid,
                                                        bool bval) {
    dbproto::DbResponse r;
    r.reqid = reqid;
    r.ok    = true;
    r.bval  = bval;
    return dbproto::build_response(r);
}

#endif // DBPROTO_TEST_HPP
