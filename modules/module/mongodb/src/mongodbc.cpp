#include <vector>

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "json.hpp"
#include "mongodbc.hpp"

namespace {

constexpr int kPbkdf2Iterations = 600000;
constexpr int kPbkdf2KeyLen      = 32;  // SHA-256 output

std::string base64_encode(const unsigned char *data, std::size_t len)
{
  BIO *bmem  = BIO_new(BIO_s_mem());
  BIO *b64   = BIO_new(BIO_f_base64());
  BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
  BIO *chain = BIO_push(b64, bmem);
  BIO_write(chain, data, static_cast<int>(len));
  BIO_flush(chain);
  BUF_MEM *ptr = nullptr;
  BIO_get_mem_ptr(chain, &ptr);
  std::string result(ptr->data, ptr->length);
  BIO_free_all(chain);
  return result;
}

std::vector<unsigned char> base64_decode(const std::string &encoded)
{
  if (encoded.empty()) return {};
  BIO *bmem  = BIO_new_mem_buf(encoded.data(), static_cast<int>(encoded.size()));
  BIO *b64   = BIO_new(BIO_f_base64());
  BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
  BIO *chain = BIO_push(b64, bmem);
  std::vector<unsigned char> buf(encoded.size());
  int len = BIO_read(chain, buf.data(), static_cast<int>(buf.size()));
  BIO_free_all(chain);
  if (len <= 0) return {};
  buf.resize(static_cast<std::size_t>(len));
  return buf;
}

// Serialise every document in cursor as a JSON array.
// Returns an empty string (not "[]") when the cursor has no results,
// preserving the existing contract that callers check for empty.
std::string cursor_to_json_array(mongocxx::cursor &cursor) {
  auto it = cursor.begin();
  if (it == cursor.end())
    return {};

  std::string out("[");
  for (; it != cursor.end(); ++it)
    out += bsoncxx::to_json(*it) + ",";
  out.back() = ']';
  return out;
}

} // namespace

MongodbClient::MongodbClient(const std::string &uri_str) : m_uri(uri_str) {
  m_instance = std::make_unique<mongocxx::instance>();

  mongocxx::uri uri(m_uri.c_str());
  ACE_DEBUG((LM_DEBUG,
             ACE_TEXT("%D [MongodbClient:%t] %M %N:%l database from URI: %s\n"),
             uri.database().c_str()));

  m_pool = std::make_unique<mongocxx::pool>(uri);
  m_dbName = uri.database();
}

bool MongodbClient::update_collection(const std::string &collectionName,
                                      const std::string &match,
                                      const std::string &document) {
  bsoncxx::document::value toUpdate = bsoncxx::from_json(document.c_str());
  bsoncxx::document::value filter = bsoncxx::from_json(match.c_str());

  auto conn = m_pool->acquire();
  if (!conn) {
    ACE_ERROR(
        (LM_ERROR,
         ACE_TEXT("%D [MongodbClient:%t] %M %N:%l acquiring DB client failed "
                  "for collection %s\n"),
         collectionName.c_str()));
    return false;
  }

  auto collection =
      conn->database(m_dbName.c_str()).collection(collectionName.c_str());

  mongocxx::options::bulk_write bulk_opt;
  mongocxx::write_concern wc;
  bulk_opt.ordered(false);
  wc.acknowledge_level(mongocxx::write_concern::level::k_default);
  bulk_opt.write_concern(wc);
  auto bulk = collection.create_bulk_write(bulk_opt);

  mongocxx::model::update_many upd(filter.view(), toUpdate.view());
  bulk.append(upd);

  auto result = bulk.execute();
  if (result && result->matched_count() > 0) {
    ACE_DEBUG(
        (LM_DEBUG,
         ACE_TEXT("%D [MongodbClient:%t] %M %N:%l bulk document updated: %d\n"),
         result->matched_count()));
    return true;
  }

  ACE_DEBUG((
      LM_DEBUG,
      ACE_TEXT("%D [MongodbClient:%t] %M %N:%l update matched 0 documents\n")));
  return false;
}

bool MongodbClient::delete_document(const std::string &collectionName,
                                    const std::string &doc) {
  bsoncxx::document::value filter = bsoncxx::from_json(doc.c_str());

  auto conn = m_pool->acquire();
  if (!conn) {
    ACE_ERROR(
        (LM_ERROR,
         ACE_TEXT("%D [MongodbClient:%t] %M %N:%l acquiring DB client failed "
                  "for collection %s\n"),
         collectionName.c_str()));
    return false;
  }

  auto collection =
      conn->database(m_dbName.c_str()).collection(collectionName.c_str());

  mongocxx::options::bulk_write bulk_opt;
  mongocxx::write_concern wc;
  bulk_opt.ordered(false);
  wc.acknowledge_level(mongocxx::write_concern::level::k_default);
  bulk_opt.write_concern(wc);
  auto bulk = collection.create_bulk_write(bulk_opt);

  mongocxx::model::delete_many del(filter.view());
  bulk.append(del);

  auto result = bulk.execute();
  if (result) {
    ACE_DEBUG((
        LM_DEBUG,
        ACE_TEXT("%D [MongodbClient:%t] %M %N:%l bulk documents deleted: %d\n"),
        result->deleted_count()));
    return true;
  }

  return false;
}

std::string MongodbClient::get_document(const std::string &collectionName,
                                        const std::string &query,
                                        const std::string &fieldProjection) {
  auto conn = m_pool->acquire();
  if (!conn) {
    ACE_ERROR(
        (LM_ERROR,
         ACE_TEXT("%D [MongodbClient:%t] %M %N:%l acquiring DB client failed "
                  "for collection %s\n"),
         collectionName.c_str()));
    return {};
  }

  auto collection =
      conn->database(m_dbName.c_str()).collection(collectionName.c_str());

  mongocxx::options::find opts{};
  opts.max_time(std::chrono::milliseconds(5000))
      .no_cursor_timeout(false)
      .projection(bsoncxx::from_json(fieldProjection.c_str()));

  auto cursor = collection.find(bsoncxx::from_json(query.c_str()).view(), opts);
  auto iter = cursor.begin();
  if (iter == cursor.end())
    return {};

  return bsoncxx::to_json(*iter);
}

std::string MongodbClient::get_documents(const std::string &collectionName,
                                         const std::string &query,
                                         const std::string &fieldProjection) {
  auto conn = m_pool->acquire();
  if (!conn) {
    ACE_ERROR(
        (LM_ERROR,
         ACE_TEXT("%D [MongodbClient:%t] %M %N:%l acquiring DB client failed "
                  "for collection %s\n"),
         collectionName.c_str()));
    return {};
  }

  auto collection =
      conn->database(m_dbName.c_str()).collection(collectionName.c_str());

  mongocxx::options::find opts{};
  opts.max_time(std::chrono::milliseconds(5000))
      .no_cursor_timeout(false)
      .projection(bsoncxx::from_json(fieldProjection.c_str()));

  auto cursor = collection.find(bsoncxx::from_json(query.c_str()).view(), opts);
  return cursor_to_json_array(cursor);
}

std::string MongodbClient::get_documents(const std::string &collectionName,
                                         const std::string &projection) {
  return get_documents(collectionName, "{}", projection);
}

std::string MongodbClient::create_document(const std::string &dbName,
                                           const std::string &collectionName,
                                           const std::string &doc) {
  bsoncxx::document::value document = bsoncxx::from_json(doc.c_str());

  auto conn = m_pool->acquire();
  if (!conn) {
    ACE_ERROR(
        (LM_ERROR,
         ACE_TEXT("%D [MongodbClient:%t] %M %N:%l acquiring DB client failed "
                  "for collection: %s\n"),
         collectionName.c_str()));
    return {};
  }

  auto collection =
      conn->database(dbName.c_str()).collection(collectionName.c_str());
  auto result = collection.insert_one(document.view());

  if (result) {
    std::string oid = result->inserted_id().get_oid().value.to_string();
    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("%D [MongodbClient:%t] %M %N:%l inserted OID: %s\n"),
               oid.c_str()));
    return oid;
  }

  return {};
}

std::int32_t
MongodbClient::create_bulk_document(const std::string &dbName,
                                    const std::string &collectionName,
                                    const std::string &doc) {
  auto conn = m_pool->acquire();
  if (!conn) {
    ACE_ERROR(
        (LM_ERROR,
         ACE_TEXT("%D [MongodbClient:%t] %M %N:%l acquiring DB client failed "
                  "for collection: %s\n"),
         collectionName.c_str()));
    return 0;
  }

  auto collection =
      conn->database(dbName.c_str()).collection(collectionName.c_str());

  // Parse the JSON array and convert each element to a BSON document.
  std::vector<bsoncxx::document::value> docs;
  try {
    auto arr = nlohmann::json::parse(doc);
    for (auto &elem : arr) {
      if (!elem.is_object()) continue;
      docs.push_back(bsoncxx::from_json(elem.dump()));
    }
  } catch (const std::exception &ex) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [MongodbClient:%t] %M %N:%l JSON parse failed: %s\n"),
               ex.what()));
    return 0;
  }

  if (docs.empty()) return 0;

  std::vector<bsoncxx::document::view> views;
  views.reserve(docs.size());
  for (auto &d : docs) views.push_back(d.view());

  mongocxx::options::insert insert_opt;
  insert_opt.ordered(false);

  auto result = collection.insert_many(views, insert_opt);
  if (!result) return 0;

  std::int32_t cnt = static_cast<std::int32_t>(result->inserted_count());
  ACE_DEBUG(
      (LM_DEBUG,
       ACE_TEXT("%D [MongodbClient:%t] %M %N:%l bulk documents created: %d\n"),
       cnt));
  return cnt;
}

std::int32_t
MongodbClient::update_bulk_document(const std::string &collectionName,
                                    const std::vector<std::string> &filter,
                                    const std::vector<std::string> &value) {
  auto conn = m_pool->acquire();
  if (!conn) {
    ACE_ERROR(
        (LM_ERROR,
         ACE_TEXT("%D [MongodbClient:%t] %M %N:%l acquiring DB client failed "
                  "for collection: %s\n"),
         collectionName.c_str()));
    return 0;
  }

  auto collection =
      conn->database(m_dbName.c_str()).collection(collectionName.c_str());

  mongocxx::options::bulk_write bulk_opt;
  mongocxx::write_concern wc;
  bulk_opt.ordered(false);
  wc.acknowledge_level(mongocxx::write_concern::level::k_default);
  bulk_opt.write_concern(wc);
  auto bulk = collection.create_bulk_write(bulk_opt);

  for (std::size_t i = 0; i < filter.size(); ++i) {
    bsoncxx::document::value filter_doc = bsoncxx::from_json(filter[i].c_str());
    bsoncxx::document::value value_doc = bsoncxx::from_json(value[i].c_str());
    ACE_DEBUG(
        (LM_DEBUG,
         ACE_TEXT("%D [MongodbClient:%t] %M %N:%l filter: %s value: %s\n"),
         filter[i].c_str(), value[i].c_str()));
    mongocxx::model::update_one upd(filter_doc.view(), value_doc.view());
    bulk.append(upd);
  }

  auto result = bulk.execute();
  if (!result)
    return 0;

  std::int32_t cnt = result->modified_count();
  ACE_DEBUG(
      (LM_DEBUG,
       ACE_TEXT("%D [MongodbClient:%t] %M %N:%l bulk documents updated: %d\n"),
       cnt));
  return cnt;
}

std::string
MongodbClient::get_access_token_for_ajoul(const std::string &json_obj) {
  bsoncxx::document::value doc_val = bsoncxx::from_json(json_obj.c_str());
  bsoncxx::document::view doc = doc_val.view();

  auto it = doc.find("access_token");
  if (it == doc.end()) {
    ACE_ERROR((LM_ERROR, ACE_TEXT("%D [MongodbClient:%t] %M %N:%l element "
                                  "access_token not found\n")));
    return {};
  }

  bsoncxx::document::element elm = *it;
  if (elm && elm.type() == bsoncxx::type::k_utf8)
    return std::string(elm.get_utf8().value.data(),
                       elm.get_utf8().value.length());

  return {};
}

std::string
MongodbClient::get_tracking_no_for_ajoul(const std::string &json_obj,
                                         std::string &reference_no) {
  bsoncxx::document::value doc_val = bsoncxx::from_json(json_obj.c_str());
  bsoncxx::document::view doc = doc_val.view();

  auto it = doc.find("Shipment");
  if (it == doc.end()) {
    ACE_ERROR(
        (LM_ERROR,
         ACE_TEXT(
             "%D [MongodbClient:%t] %M %N:%l Shipment object not found\n")));
    return {};
  }

  auto tt = it->get_document().value;
  reference_no = tt["reference"].get_utf8().value.to_string();
  ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [worker:%t] %M %N:%l reference: %s\n"),
             reference_no.c_str()));

  bsoncxx::document::element elm = doc["TrackingNumber"];
  if (elm && elm.type() == bsoncxx::type::k_utf8)
    return std::string(elm.get_utf8().value.data(),
                       elm.get_utf8().value.length());

  return {};
}

std::string MongodbClient::next_awbno(const std::string &prefix) {
  auto conn = m_pool->acquire();
  if (!conn) {
    ACE_ERROR(
        (LM_ERROR,
         ACE_TEXT("%D [MongodbClient:%t] %M %N:%l acquiring DB client failed "
                  "for next_awbno\n")));
    return {};
  }

  try {
    auto collection = conn->database(m_dbName.c_str()).collection("counters");

    // Atomically increment the counter; upsert creates the document with
    // seq=1 on the very first call.
    auto filter = bsoncxx::builder::stream::document{}
                  << "_id" << "awbno" << bsoncxx::builder::stream::finalize;

    auto update = bsoncxx::builder::stream::document{}
                  << "$inc" << bsoncxx::builder::stream::open_document << "seq"
                  << std::int64_t(1) << bsoncxx::builder::stream::close_document
                  << bsoncxx::builder::stream::finalize;

    mongocxx::options::find_one_and_update opts;
    opts.upsert(true);
    opts.return_document(mongocxx::options::return_document::k_after);

    auto result =
        collection.find_one_and_update(filter.view(), update.view(), opts);

    if (result) {
      auto seq_elm = result->view()["seq"];
      std::int64_t seq =
          (seq_elm.type() == bsoncxx::type::k_int64)
              ? seq_elm.get_int64().value
              : static_cast<std::int64_t>(seq_elm.get_int32().value);
      std::ostringstream oss;
      oss << prefix << std::setfill('0') << std::setw(9) << seq;
      ACE_DEBUG((LM_DEBUG,
                 ACE_TEXT("%D [MongodbClient:%t] %M %N:%l next_awbno: %s\n"),
                 oss.str().c_str()));
      return oss.str();
    }
  } catch (const std::exception &e) {
    ACE_ERROR(
        (LM_ERROR,
         ACE_TEXT("%D [MongodbClient:%t] %M %N:%l next_awbno failed: %s\n"),
         e.what()));
  }
  return {};
}

std::string MongodbClient::store_file(const std::string &filename,
                                      const std::string &content_type,
                                      const std::vector<std::uint8_t> &data) {
  auto conn = m_pool->acquire();
  if (!conn) {
    ACE_ERROR(
        (LM_ERROR,
         ACE_TEXT("%D [MongodbClient:%t] %M %N:%l acquiring DB client failed "
                  "for GridFS store_file\n")));
    return {};
  }

  try {
    auto db = conn->database(m_dbName.c_str());
    auto bucket = db.gridfs_bucket();

    // Generate the file OID upfront so we can return it without parsing
    // the result of close() whose return type varies across driver versions.
    bsoncxx::oid id;
    bsoncxx::types::bson_value::view id_view{bsoncxx::types::b_oid{id}};

    auto meta = bsoncxx::builder::stream::document{}
                << "contentType" << content_type
                << bsoncxx::builder::stream::finalize;

    mongocxx::options::gridfs::upload opts;
    opts.metadata(meta.view());

    auto uploader = bucket.open_upload_stream_with_id(id_view, filename, opts);
    uploader.write(data.data(), data.size());
    uploader.close();

    ACE_DEBUG(
        (LM_DEBUG,
         ACE_TEXT("%D [MongodbClient:%t] %M %N:%l stored file '%s' (%zu bytes) "
                  "OID: %s\n"),
         filename.c_str(), data.size(), id.to_string().c_str()));
    return id.to_string();
  } catch (const std::exception &e) {
    ACE_ERROR(
        (LM_ERROR,
         ACE_TEXT("%D [MongodbClient:%t] %M %N:%l store_file failed: %s\n"),
         e.what()));
    return {};
  }
}

std::vector<std::uint8_t>
MongodbClient::fetch_file(const std::string &filename) {
  auto conn = m_pool->acquire();
  if (!conn) {
    ACE_ERROR(
        (LM_ERROR,
         ACE_TEXT("%D [MongodbClient:%t] %M %N:%l acquiring DB client failed "
                  "for GridFS fetch_file\n")));
    return {};
  }

  try {
    auto db = conn->database(m_dbName.c_str());
    auto bucket = db.gridfs_bucket();

    // mongocxx v3.6 has no open_download_stream_by_name; find the file
    // metadata first to obtain its _id, then open by id.
    // options::gridfs::find is also absent in v3.6, so we iterate and
    // stop at the first match instead of passing a limit option.
    auto name_filter = bsoncxx::builder::stream::document{}
                       << "filename" << filename
                       << bsoncxx::builder::stream::finalize;
    auto cursor = bucket.find(name_filter.view());
    auto it = cursor.begin();
    if (it == cursor.end())
      throw std::runtime_error("file not found: " + filename);

    bsoncxx::types::bson_value::view id_view = (*it)["_id"].get_value();
    auto downloader = bucket.open_download_stream(id_view);

    auto file_len = static_cast<std::size_t>(downloader.file_length());
    std::vector<std::uint8_t> buf(file_len);
    downloader.read(buf.data(), file_len);

    ACE_DEBUG(
        (LM_DEBUG,
         ACE_TEXT(
             "%D [MongodbClient:%t] %M %N:%l fetched file '%s' (%zu bytes)\n"),
         filename.c_str(), buf.size()));
    return buf;
  } catch (const std::exception &e) {
    ACE_ERROR((
        LM_ERROR,
        ACE_TEXT("%D [MongodbClient:%t] %M %N:%l fetch_file '%s' failed: %s\n"),
        filename.c_str(), e.what()));
    return {};
  }
}

std::vector<std::uint8_t>
MongodbClient::fetch_file_by_id(const std::string &oid_str) {
  auto conn = m_pool->acquire();
  if (!conn) {
    ACE_ERROR(
        (LM_ERROR,
         ACE_TEXT("%D [MongodbClient:%t] %M %N:%l acquiring DB client failed "
                  "for GridFS fetch_file_by_id\n")));
    return {};
  }

  try {
    auto db = conn->database(m_dbName.c_str());
    auto bucket = db.gridfs_bucket();

    bsoncxx::oid id(oid_str);
    bsoncxx::types::bson_value::view id_view{bsoncxx::types::b_oid{id}};
    auto downloader = bucket.open_download_stream(id_view);

    auto file_len = static_cast<std::size_t>(downloader.file_length());
    std::vector<std::uint8_t> buf(file_len);
    downloader.read(buf.data(), file_len);

    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("%D [MongodbClient:%t] %M %N:%l fetched file OID '%s' "
                        "(%zu bytes)\n"),
               oid_str.c_str(), buf.size()));
    return buf;
  } catch (const std::exception &e) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [MongodbClient:%t] %M %N:%l fetch_file_by_id '%s' "
                        "failed: %s\n"),
               oid_str.c_str(), e.what()));
    return {};
  }
}

bool MongodbClient::delete_file(const std::string &oid_str) {
  auto conn = m_pool->acquire();
  if (!conn) {
    ACE_ERROR(
        (LM_ERROR,
         ACE_TEXT("%D [MongodbClient:%t] %M %N:%l acquiring DB client failed "
                  "for GridFS delete_file\n")));
    return false;
  }

  try {
    auto db = conn->database(m_dbName.c_str());
    auto bucket = db.gridfs_bucket();

    bsoncxx::oid id(oid_str);
    bsoncxx::types::bson_value::view id_view{bsoncxx::types::b_oid{id}};
    bucket.delete_file(id_view);

    ACE_DEBUG(
        (LM_DEBUG,
         ACE_TEXT(
             "%D [MongodbClient:%t] %M %N:%l deleted GridFS file OID '%s'\n"),
         oid_str.c_str()));
    return true;
  } catch (const std::exception &e) {
    ACE_ERROR(
        (LM_ERROR,
         ACE_TEXT(
             "%D [MongodbClient:%t] %M %N:%l delete_file '%s' failed: %s\n"),
         oid_str.c_str(), e.what()));
    return false;
  }
}

JsonExtract MongodbClient::from_json(const std::string &json_obj,
                                     const std::string &key) const {
  bsoncxx::document::value doc_val = bsoncxx::from_json(json_obj.c_str());
  bsoncxx::document::view doc = doc_val.view();

  auto it = doc.find(key);
  if (it == doc.end()) {
    ACE_ERROR(
        (LM_ERROR,
         ACE_TEXT("%D [MongodbClient:%t] %M %N:%l element '%s' not found\n"),
         key.c_str()));
    return std::monostate{};
  }

  bsoncxx::document::element elm = *it;

  // UTF-8 scalar → std::string
  if (elm.type() == bsoncxx::type::k_utf8)
    return std::string(elm.get_utf8().value.data(),
                       elm.get_utf8().value.length());

  if (elm.type() == bsoncxx::type::k_array) {
    bsoncxx::array::view arr = elm.get_array().value;
    auto first = arr.begin();
    if (first == arr.end())
      return JsonStrVec{};

    // Array of strings → JsonStrVec
    if (first->type() == bsoncxx::type::k_utf8) {
      JsonStrVec vec;
      for (bsoncxx::array::element entry : arr) {
        if (entry.type() == bsoncxx::type::k_utf8)
          vec.emplace_back(entry.get_utf8().value.data(),
                           entry.get_utf8().value.length());
      }
      return vec;
    }

    // Array of sub-documents → JsonDocList ({file-name, file-content} pairs)
    if (first->type() == bsoncxx::type::k_document) {
      JsonDocList list;
      for (bsoncxx::array::element entry : arr) {
        if (entry.type() == bsoncxx::type::k_document) {
          auto subdoc = entry.get_document().value;
          list.emplace_back(subdoc["file-name"].get_utf8().value.data(),
                            subdoc["file-content"].get_utf8().value.data());
        }
      }
      return list;
    }
  }

  ACE_ERROR((LM_ERROR,
             ACE_TEXT("%D [MongodbClient:%t] %M %N:%l element '%s' has "
                      "unsupported BSON type\n"),
             key.c_str()));
  return std::monostate{};
}

// ── Password hashing ───────────────────────────────────────────────────────────

std::string MongodbClient::hash_password(const std::string &password)
{
  unsigned char salt[16];
  RAND_bytes(salt, sizeof(salt));

  unsigned char key[kPbkdf2KeyLen];
  PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()),
                    salt, static_cast<int>(sizeof(salt)),
                    kPbkdf2Iterations,
                    EVP_sha256(),
                    kPbkdf2KeyLen, key);

  std::string salt_b64 = base64_encode(salt, sizeof(salt));
  std::string key_b64  = base64_encode(key, sizeof(key));

  std::ostringstream oss;
  oss << "$pbkdf2-sha256$i=" << kPbkdf2Iterations
      << "$" << salt_b64 << "$" << key_b64;
  return oss.str();
}

bool MongodbClient::verify_password(const std::string &password,
                                    const std::string &stored_hash)
{
  if (stored_hash.empty()) return false;

  // Split by '$' — format: $pbkdf2-sha256$i=<n>$<salt-b64>$<hash-b64>
  std::vector<std::string> parts;
  std::size_t pos = 0;
  while (pos < stored_hash.size()) {
    std::size_t next = stored_hash.find('$', pos);
    if (next == std::string::npos) {
      parts.push_back(stored_hash.substr(pos));
      break;
    }
    if (next > pos)
      parts.push_back(stored_hash.substr(pos, next - pos));
    pos = next + 1;
  }

  if (parts.size() < 4) return false;
  if (parts[0] != "pbkdf2-sha256") return false;

  int iterations = kPbkdf2Iterations;
  if (parts[1].find("i=") == 0)
    iterations = std::stoi(parts[1].substr(2));

  auto salt_bytes = base64_decode(parts[2]);
  auto hash_bytes = base64_decode(parts[3]);
  if (salt_bytes.empty() || hash_bytes.empty()) return false;

  unsigned char key[kPbkdf2KeyLen];
  PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()),
                    salt_bytes.data(), static_cast<int>(salt_bytes.size()),
                    iterations,
                    EVP_sha256(),
                    kPbkdf2KeyLen, key);

  return CRYPTO_memcmp(key, hash_bytes.data(), hash_bytes.size()) == 0;
}

int migrate_account_passwords(IMongodbClient &db) {
  std::string result = db.get_documents("account", "{}");
  if (result.empty())
    return 0;

  nlohmann::json docs;
  try {
    docs = nlohmann::json::parse(result);
  } catch (...) {
    return 0;
  }

  if (!docs.is_array())
    return 0;

  int count = 0;
  for (auto &doc : docs) {
    if (!doc.contains("loginCredentials") || !doc["loginCredentials"].is_object())
      continue;

    auto &lc = doc["loginCredentials"];

    // Already has a hash — skip
    if (lc.contains("passwordHash"))
      continue;

    // No plain-text password to migrate — skip
    if (!lc.contains("accountPassword") || !lc["accountPassword"].is_string())
      continue;

    std::string plain = lc["accountPassword"].get<std::string>();
    if (plain.empty())
      continue;

    std::string hash = MongodbClient::hash_password(plain);

    nlohmann::json filter = nlohmann::json::object();
    if (lc.contains("accountCode") && lc["accountCode"].is_string())
      filter = {{"loginCredentials.accountCode", lc["accountCode"].get<std::string>()}};

    nlohmann::json update = {
        {"$set", {{"loginCredentials.passwordHash", hash}}},
        {"$unset", {{"loginCredentials.accountPassword", ""}}}};

    if (db.update_collection("account", filter.dump(), update.dump()))
      count++;
  }

  return count;
}
