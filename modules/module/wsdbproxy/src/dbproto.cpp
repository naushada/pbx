#include "dbproto.hpp"

#include <atomic>
#include <stdexcept>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view.hpp>
#include <bsoncxx/types.hpp>

using namespace bsoncxx::builder::basic;

namespace dbproto {

namespace {

static std::atomic<std::int32_t> s_reqid{0};

bsoncxx::types::b_binary make_bin(const std::vector<std::uint8_t>& v) {
  return bsoncxx::types::b_binary{
      bsoncxx::binary_sub_type::k_binary,
      static_cast<std::uint32_t>(v.size()),
      v.data()};
}

std::vector<std::uint8_t> view_to_vec(bsoncxx::document::view v) {
  return {v.data(), v.data() + v.length()};
}

} // namespace

// ── build_request ─────────────────────────────────────────────────────────────

std::pair<std::int32_t, std::vector<std::uint8_t>>
build_request(DbOp op,
              const std::string& db,
              const std::string& coll,
              const std::vector<std::uint8_t>& doc,
              const std::vector<std::uint8_t>& doc2,
              const std::string& sval)
{
  std::int32_t reqid = s_reqid.fetch_add(1, std::memory_order_relaxed);

  auto bdoc = bsoncxx::builder::basic::document{};
  bdoc.append(kvp("reqid", reqid));
  bdoc.append(kvp("op",    static_cast<std::int32_t>(op)));

  if (!db.empty())   bdoc.append(kvp("db",   db));
  if (!coll.empty()) bdoc.append(kvp("coll", coll));
  if (!doc.empty())  bdoc.append(kvp("doc",  make_bin(doc)));
  if (!doc2.empty()) bdoc.append(kvp("doc2", make_bin(doc2)));
  if (!sval.empty()) bdoc.append(kvp("sval", sval));

  return {reqid, view_to_vec(bdoc.view())};
}

// ── parse_request ─────────────────────────────────────────────────────────────

bool parse_request(const std::vector<std::uint8_t>& bytes, DbRequest& out)
{
  if (bytes.empty()) return false;
  try {
    bsoncxx::document::view v{bytes.data(), bytes.size()};

    if (auto e = v["reqid"]) out.reqid = e.get_int32().value;
    if (auto e = v["op"])    out.op    = static_cast<DbOp>(e.get_int32().value);
    if (auto e = v["db"])    out.db    = std::string(e.get_utf8().value);
    if (auto e = v["coll"])  out.coll  = std::string(e.get_utf8().value);
    if (auto e = v["sval"])  out.sval  = std::string(e.get_utf8().value);

    if (auto e = v["doc"]) {
      auto b = e.get_binary();
      out.doc.assign(b.bytes, b.bytes + b.size);
    }
    if (auto e = v["doc2"]) {
      auto b = e.get_binary();
      out.doc2.assign(b.bytes, b.bytes + b.size);
    }
    return true;
  } catch (...) {
    return false;
  }
}

// ── build_response ────────────────────────────────────────────────────────────

std::vector<std::uint8_t> build_response(const DbResponse& rsp)
{
  auto bdoc = bsoncxx::builder::basic::document{};
  bdoc.append(kvp("reqid", rsp.reqid));
  bdoc.append(kvp("ok",    rsp.ok));

  if (!rsp.sval.empty())   bdoc.append(kvp("sval",   rsp.sval));
  if (rsp.ival != 0)       bdoc.append(kvp("ival",   rsp.ival));
  if (rsp.bval)            bdoc.append(kvp("bval",   rsp.bval));
  if (!rsp.data.empty())   bdoc.append(kvp("data",   make_bin(rsp.data)));
  if (!rsp.errmsg.empty()) bdoc.append(kvp("errmsg", rsp.errmsg));

  return view_to_vec(bdoc.view());
}

// ── parse_response ────────────────────────────────────────────────────────────

bool parse_response(const std::vector<std::uint8_t>& bytes, DbResponse& out)
{
  if (bytes.empty()) return false;
  try {
    bsoncxx::document::view v{bytes.data(), bytes.size()};

    if (auto e = v["reqid"])  out.reqid  = e.get_int32().value;
    if (auto e = v["ok"])     out.ok     = e.get_bool().value;
    if (auto e = v["sval"])   out.sval   = std::string(e.get_utf8().value);
    if (auto e = v["ival"])   out.ival   = e.get_int32().value;
    if (auto e = v["bval"])   out.bval   = e.get_bool().value;
    if (auto e = v["errmsg"]) out.errmsg = std::string(e.get_utf8().value);

    if (auto e = v["data"]) {
      auto b = e.get_binary();
      out.data.assign(b.bytes, b.bytes + b.size);
    }
    return true;
  } catch (...) {
    return false;
  }
}

// ── make_error_response ───────────────────────────────────────────────────────

std::vector<std::uint8_t> make_error_response(std::int32_t reqid,
                                              const std::string& errmsg)
{
  DbResponse rsp;
  rsp.reqid  = reqid;
  rsp.ok     = false;
  rsp.errmsg = errmsg;
  return build_response(rsp);
}

} // namespace dbproto
