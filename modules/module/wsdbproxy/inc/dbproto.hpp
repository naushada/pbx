#ifndef DBPROTO_HPP
#define DBPROTO_HPP

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/// MongoDB operation identifiers carried in the request envelope.
enum class DbOp : std::int32_t {
  CREATE_DOCUMENT       = 0,
  CREATE_BULK_DOCUMENT  = 1,
  UPDATE_COLLECTION     = 2,
  UPDATE_BULK_DOCUMENT  = 3,
  DELETE_DOCUMENT       = 4,
  GET_DOCUMENT          = 5,
  GET_DOCUMENTS_QUERIED = 6,
  GET_DOCUMENTS_ALL     = 7,
  NEXT_AWBNO            = 8,
  STORE_FILE            = 9,
  FETCH_FILE            = 10,
  FETCH_FILE_BY_ID      = 11,
  DELETE_FILE           = 12,
};

namespace dbproto {

/**
 * @brief Build a BSON request envelope.
 *
 * Assigns a monotonically incrementing reqid internally.
 *
 * @param op   Operation code.
 * @param db   Database name (may be empty when not required by the op).
 * @param coll Collection name (may be empty when not required).
 * @param doc  Primary BSON payload bytes (query / filter / insert body).
 * @param doc2 Secondary BSON payload bytes (projection / update document).
 * @param sval String parameter (AWB prefix, file name, OID, etc.).
 * @return {reqid, bson_bytes}  The assigned request ID and the serialised
 *         BSON document ready to embed as a WebSocket binary frame payload.
 */
std::pair<std::int32_t, std::vector<std::uint8_t>>
build_request(DbOp op,
              const std::string& db   = {},
              const std::string& coll = {},
              const std::vector<std::uint8_t>& doc  = {},
              const std::vector<std::uint8_t>& doc2 = {},
              const std::string& sval = {});

/// Result of parsing a BSON response envelope.
struct DbResponse {
  std::int32_t             reqid  {-1};
  bool                     ok     {false};
  std::string              sval;
  std::int32_t             ival   {0};
  bool                     bval   {false};
  std::vector<std::uint8_t> data;
  std::string              errmsg;
};

/**
 * @brief Parse a BSON response envelope received from the agent.
 * @return true on success; false if the bytes are malformed or empty.
 */
bool parse_response(const std::vector<std::uint8_t>& bytes, DbResponse& out);

/**
 * @brief Build a BSON response envelope (used by ws-db-agent and for
 *        synthesising error responses on the server side).
 */
std::vector<std::uint8_t> build_response(const DbResponse& rsp);

/// Result of parsing a BSON request envelope (used by ws-db-agent).
struct DbRequest {
  std::int32_t             reqid  {-1};
  DbOp                     op     {DbOp::GET_DOCUMENT};
  std::string              db;
  std::string              coll;
  std::vector<std::uint8_t> doc;
  std::vector<std::uint8_t> doc2;
  std::string              sval;
};

bool parse_request(const std::vector<std::uint8_t>& bytes, DbRequest& out);

/// Synthesise an error response with the given message, for the given reqid.
std::vector<std::uint8_t> make_error_response(std::int32_t reqid,
                                              const std::string& errmsg);

} // namespace dbproto

#endif // DBPROTO_HPP
