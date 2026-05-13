#ifndef MONGODBC_HPP
#define MONGODBC_HPP

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <tuple>
#include <variant>
#include <vector>

#include <bsoncxx/builder/stream/array.hpp>
#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/builder/stream/helpers.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/stdx/string_view.hpp>
#include <bsoncxx/string/to_string.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/bulk_write.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/cursor.hpp>
#include <mongocxx/gridfs/bucket.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/find_one_and_update.hpp>
#include <mongocxx/options/gridfs/upload.hpp>
#include <mongocxx/pool.hpp>
#include <mongocxx/stdx.hpp>
#include <mongocxx/uri.hpp>

#include "ace/Log_Msg.h"

/// Flat vector of UTF-8 strings extracted from a JSON array element.
using JsonStrVec = std::vector<std::string>;

/// Vector of (file-name, file-content) pairs extracted from a JSON document array.
using JsonDocList = std::vector<std::tuple<std::string, std::string>>;

/**
 * @brief Result type for @c MongodbClient::from_json().
 *
 * Alternatives:
 *  - @c std::monostate — key not found or element type not supported.
 *  - @c std::string    — the element is a UTF-8 string (@c k_utf8).
 *  - @c JsonStrVec     — the element is an array of UTF-8 strings.
 *  - @c JsonDocList    — the element is an array of sub-documents
 *                        each containing @c file-name and @c file-content fields.
 */
using JsonExtract = std::variant<std::monostate, std::string, JsonStrVec, JsonDocList>;

/**
 * @brief Abstract interface for MongoDB CRUD operations.
 *
 * Both the local MongodbClient (direct pool) and WsMongodbProxy (WebSocket
 * tunnel) implement this interface so that MicroService workers are agnostic
 * to whether MongoDB is local or remote.
 */
class IMongodbClient {
public:
  virtual ~IMongodbClient() = default;

  virtual const std::string &get_database() const = 0;

  virtual std::string create_document(const std::string &dbName,
                                      const std::string &coll,
                                      const std::string &doc) = 0;

  virtual std::int32_t create_bulk_document(const std::string &dbName,
                                            const std::string &coll,
                                            const std::string &doc) = 0;

  virtual bool update_collection(const std::string &coll,
                                 const std::string &filter,
                                 const std::string &document) = 0;

  virtual std::int32_t update_bulk_document(
      const std::string &coll,
      const std::vector<std::string> &filter,
      const std::vector<std::string> &value) = 0;

  virtual bool delete_document(const std::string &coll,
                               const std::string &doc) = 0;

  virtual std::string get_document(const std::string &coll,
                                   const std::string &query,
                                   const std::string &projection) = 0;

  virtual std::string get_documents(const std::string &coll,
                                    const std::string &query,
                                    const std::string &projection) = 0;

  virtual std::string get_documents(const std::string &coll,
                                    const std::string &projection) = 0;

  virtual std::string next_awbno(const std::string &prefix = "AWB") = 0;

  virtual std::string store_file(const std::string &filename,
                                 const std::string &content_type,
                                 const std::vector<std::uint8_t> &data) = 0;

  virtual std::vector<std::uint8_t> fetch_file(const std::string &filename) = 0;

  virtual std::vector<std::uint8_t> fetch_file_by_id(const std::string &oid) = 0;

  virtual bool delete_file(const std::string &oid) = 0;
};

/**
 * @brief MongoDB connection-pool client.
 *
 * Wraps a mongocxx::pool so that multiple worker threads can share a single
 * MongodbClient instance and acquire independent client handles without
 * synchronization overhead.  All public methods acquire a connection from the
 * pool, perform the operation, and release it automatically on return.
 *
 * @par Construction
 * @code
 *   MongodbClient db("mongodb://localhost:27017/mydb?maxPoolSize=10");
 *   std::string oid = db.create_document(db.get_database(), "orders", jsonDoc);
 * @endcode
 */
class MongodbClient : public IMongodbClient {
public:
  /**
   * @brief Construct and initialise the connection pool from a MongoDB URI.
   * @param uri  Full MongoDB connection string including database name and
   *             optional pool-size parameters, e.g.
   *             @c "mongodb://host/db?maxPoolSize=20".
   */
  explicit MongodbClient(const std::string &uri);

  /// Default-construct an uninitialised client (pool is null).
  MongodbClient() = default;
  ~MongodbClient() = default;

  /// Return the database name extracted from the URI at construction time.
  const std::string &get_database() const override { return m_dbName; }

  /// Override the active database name.
  void set_database(const std::string &dbName) { m_dbName = dbName; }

  /// Return the connection URI string.
  const std::string &get_uri() const { return m_uri; }

  /** @name Write operations */
  ///@{
  /**
   * @brief Insert a single document and return its inserted OID as a string.
   * @param dbName  Target database name.
   * @param coll    Target collection name.
   * @param doc     JSON document string to insert.
   * @return Inserted OID string, or empty on failure.
   */
  std::string create_document(const std::string &dbName,
                              const std::string &coll, const std::string &doc) override;

  /**
   * @brief Insert multiple documents via a bulk-write operation.
   * @param dbName  Target database name.
   * @param coll    Target collection name.
   * @param doc     JSON object whose values are the documents to insert.
   * @return Number of documents inserted.
   */
  std::int32_t create_bulk_document(const std::string &dbName,
                                    const std::string &coll,
                                    const std::string &doc) override;

  /**
   * @brief Update all documents matching @p filter in @p coll.
   * @param coll      Collection name.
   * @param filter    JSON match filter.
   * @param document  JSON update document (e.g. @c {"$set": {...}}).
   * @return @c true if at least one document was matched and updated.
   */
  bool update_collection(const std::string &coll, const std::string &filter,
                         const std::string &document) override;

  /**
   * @brief Bulk-update multiple documents using parallel filter/value arrays.
   * @param coll    Collection name.
   * @param filter  Per-document match filters.
   * @param value   Per-document update documents (parallel to @p filter).
   * @return Number of documents modified.
   */
  std::int32_t update_bulk_document(const std::string &coll,
                                    const std::vector<std::string> &filter,
                                    const std::vector<std::string> &value) override;

  /**
   * @brief Delete all documents matching @p doc filter from @p coll.
   * @param coll  Collection name.
   * @param doc   JSON match filter.
   * @return @c true if the bulk operation completed successfully.
   */
  bool delete_document(const std::string &coll, const std::string &doc) override;
  ///@}

  /** @name Read operations */
  ///@{
  /**
   * @brief Fetch the first document matching @p query.
   * @param coll        Collection name.
   * @param query       JSON match filter.
   * @param projection  JSON projection document.
   * @return First matching document as a JSON string, or empty if none found.
   */
  std::string get_document(const std::string &coll, const std::string &query,
                           const std::string &projection) override;

  /**
   * @brief Fetch all documents matching @p query as a JSON array string.
   * @param coll        Collection name.
   * @param query       JSON match filter.
   * @param projection  JSON projection document.
   * @return JSON array of matching documents, or empty if none found.
   */
  std::string get_documents(const std::string &coll, const std::string &query,
                            const std::string &projection) override;

  /**
   * @brief Fetch all documents in @p coll (no filter) as a JSON array string.
   * @param coll        Collection name.
   * @param projection  JSON projection document.
   * @return JSON array of all documents, or empty if the collection is empty.
   */
  std::string get_documents(const std::string &coll,
                            const std::string &projection) override;
  ///@}

  /** @name Vendor-specific helpers (Ajoul courier API) */
  ///@{
  /**
   * @brief Extract the @c access_token string from a JSON response object.
   * @param json_obj  Raw JSON string from the Ajoul auth endpoint.
   * @return Token string, or empty if the key is absent or has a non-string
   * type.
   */
  std::string get_access_token_for_ajoul(const std::string &json_obj);

  /**
   * @brief Extract the tracking number and reference from a JSON shipment
   * response.
   * @param json_obj      Raw JSON string from the Ajoul create-shipment
   * endpoint.
   * @param reference_no  Output: populated with the shipment reference string.
   * @return Tracking number string, or empty on failure.
   */
  std::string get_tracking_no_for_ajoul(const std::string &json_obj,
                                        std::string &reference_no);
  ///@}

  /** @name AWB number generation */
  ///@{
  /**
   * @brief Atomically allocate the next AWB (Air Waybill) number.
   *
   * Uses the @b counter-collection pattern: a document
   * @c {"_id":"awbno","seq":<n>} in the @c counters collection is atomically
   * incremented with @c $inc via @c find_one_and_update (upsert).  Because
   * MongoDB performs this as a single atomic operation at the document level,
   * concurrent workers can call this method safely without generating
   * duplicate numbers or needing any application-level lock.
   *
   * The returned string has the form @c \<prefix\>NNNNNNNNN, where NNNNNNNNN is
   * a zero-padded 9-digit decimal sequence number starting at 1.  The prefix
   * defaults to @c "AWB", giving values like @c AWB000000001, @c AWB000000002,
   * etc.  Up to 999,999,999 unique numbers can be generated before wrap-around.
   *
   * @par Counter document schema (@c counters collection)
   * @code
   * { "_id": "awbno", "seq": <int64> }
   * @endcode
   *
   * @param prefix  Leading string prepended to the numeric part.
   *                Pass a carrier code (e.g. @c "DHL", @c "FDX") to issue
   *                per-carrier sequences.
   * @return Formatted AWB string (e.g. @c "AWB000000042"), or empty on error.
   */
  std::string next_awbno(const std::string &prefix = "AWB") override;
  ///@}

  /** @name GridFS file storage */
  ///@{
  /**
   * @brief Upload a binary file to GridFS.
   *
   * The file is sharded into chunks (default 255 KB) and stored in the
   * @c fs.files / @c fs.chunks collections of the active database.
   * The @p content_type string is persisted as GridFS metadata so it can be
   * recovered on download without additional lookups.
   *
   * @param filename      Logical name used to retrieve the file later.
   * @param content_type  MIME type, e.g. @c "application/pdf", @c "image/png".
   * @param data          Raw file bytes.
   * @return OID string of the stored file, or empty on failure.
   */
  std::string store_file(const std::string &filename,
                         const std::string &content_type,
                         const std::vector<std::uint8_t> &data) override;

  /**
   * @brief Download a file from GridFS by its logical name.
   * @param filename  Name used when the file was uploaded.
   * @return Raw file bytes, or empty vector if the file is not found.
   */
  std::vector<std::uint8_t> fetch_file(const std::string &filename) override;

  /**
   * @brief Download a file from GridFS by its OID.
   * @param oid  OID string returned by @c store_file().
   * @return Raw file bytes, or empty vector if the file is not found.
   */
  std::vector<std::uint8_t> fetch_file_by_id(const std::string &oid) override;

  /**
   * @brief Delete a file (chunks + metadata) from GridFS by its OID.
   * @param oid  OID string returned by @c store_file().
   * @return @c true on success, @c false if the file is not found or an
   *         error occurs.
   */
  bool delete_file(const std::string &oid) override;
  ///@}

  /** @name Password hashing */
  ///@{
  static std::string hash_password(const std::string &password);
  static bool verify_password(const std::string &password,
                              const std::string &stored_hash);
  ///@}

  /** @name JSON extraction utility */
  ///@{
  /**
   * @brief Extract a typed value from a JSON document by key.
   *
   * Inspects the BSON type of the element and returns the matching variant
   * alternative:
   *  - @c k_utf8 → @c std::string
   *  - @c k_array of strings → @c JsonStrVec
   *  - @c k_array of sub-documents (with @c file-name / @c file-content fields)
   *    → @c JsonDocList
   *  - key absent or type not matched → @c std::monostate
   *
   * @param json_obj  JSON document string.
   * @param key       Key to look up.
   * @return A @c JsonExtract variant holding the extracted value, or
   *         @c std::monostate on failure.
   */
  JsonExtract from_json(const std::string &json_obj,
                        const std::string &key) const;
  ///@}

private:
  std::string m_uri;
  std::string m_dbName;
  std::unique_ptr<mongocxx::pool> m_pool;
  std::unique_ptr<mongocxx::instance> m_instance;
};

/**
 * @brief Migrate plain-text passwords to PBKDF2 hashes in-place.
 *
 * Iterates all documents in the @c account collection.  For each document that
 * has @c loginCredentials.accountPassword and does NOT already have
 * @c loginCredentials.passwordHash, hashes the password via
 * @c MongodbClient::hash_password() and atomically updates the document with
 * @c $set and @c $unset.
 *
 * @param db  Any @c IMongodbClient implementation (real or mock).
 * @return Number of documents successfully migrated.
 */
int migrate_account_passwords(IMongodbClient &db);

#endif // MONGODBC_H
