// HandoffOrdering — source-invariant test.
//
// `WebConnection::handle_input` performs three WebSocket upgrade hand-offs
// (`/ws/db`, `/sip-ws`, `/agent`). Each must follow the exact ordering
// documented in xpmile's CLAUDE.md §"WebSocket hand-off mechanics":
//
//     m_handedOff = true                   // 1
//     m_stream.set_handle(ACE_INVALID_HANDLE) // 2
//     reactor()->remove_handler(this, …)   // 3  — must precede #4
//     m_handle = ACE_INVALID_HANDLE        // 4
//     <publish raw fd to subsystem>        // 5  — bridge / endpoint / wsDb
//     parent().connectionPool().erase(raw) // 6  — deletes `this`
//
// The fatal mistake is swapping #3 and #4. `remove_handler` calls
// `get_handle()` internally to find which fd to deregister from epoll;
// if `m_handle` is `ACE_INVALID_HANDLE` first, the deregistration is a
// no-op, the fd stays in epoll, and the reactor dispatches to the
// already-deleted `WebConnection` the next time the socket is
// readable. Crash, or worse, silent corruption.
//
// This test is a defensive guard: it reads the production source
// directly and asserts the ordering for every hand-off branch. A
// future hand who swaps the lines (an easy mistake — both lines look
// like "tear down") gets a loud failure instead of a flaky crash in
// production.
//
// Why source-grep instead of a real-reactor end-to-end? A reactor
// test would prove that the hand-off WORKS but is fundamentally
// blind to the ordering bug class: if the swap is benign on Linux
// (which it sometimes is, depending on the reactor implementation),
// the test still passes. The source ordering is the actual invariant
// we care about. See TDD-PLAN.md Layer 3 for the deferral rationale.

#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
#include <string>

namespace {

constexpr const char *kWebserviceSrc =
    "/src/modules/module/webservice/src/webservice.cpp";

std::string read_file(const std::string &path) {
  std::ifstream f(path);
  if (!f.is_open()) return {};
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

/// Locate the upgrade block for @p marker (e.g. "is_sip_ws_upgrade"),
/// return the source between the `if (...)` and the trailing `return 0;`
/// that closes the block. Empty string if the marker is missing.
std::string extract_upgrade_block(const std::string &src,
                                   const std::string &marker) {
  const auto m = src.find(marker);
  if (m == std::string::npos) return {};
  // Walk forward to the `if (...)` predicate that uses the marker, then
  // find the next `return 0;` — that's the end of the block.
  const auto if_pos = src.rfind("if (", m);
  if (if_pos == std::string::npos) return {};
  const auto ret_pos = src.find("return 0;", m);
  if (ret_pos == std::string::npos) return {};
  return src.substr(if_pos, ret_pos - if_pos);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// /sip-ws — Layer 3 hand-off into BrowserStream
// ─────────────────────────────────────────────────────────────────────────────

TEST(HandoffOrdering, SipWsBranch_RemoveHandlerBeforeClearingHandle)
{
    const std::string src = read_file(kWebserviceSrc);
    ASSERT_FALSE(src.empty())
        << "could not read " << kWebserviceSrc
        << " — test must run inside the build container where the source "
           "tree is copied to /src/";

    const std::string block = extract_upgrade_block(src, "is_sip_ws_upgrade");
    ASSERT_FALSE(block.empty()) << "could not locate the /sip-ws upgrade block";

    const auto remove_pos = block.find("remove_handler");
    const auto clear_pos  = block.find("m_handle = ACE_INVALID_HANDLE");

    ASSERT_NE(std::string::npos, remove_pos)
        << "/sip-ws branch must call remove_handler() before publishing the fd";
    ASSERT_NE(std::string::npos, clear_pos)
        << "/sip-ws branch must clear m_handle to ACE_INVALID_HANDLE";

    EXPECT_LT(remove_pos, clear_pos)
        << "remove_handler() MUST precede `m_handle = ACE_INVALID_HANDLE`. "
           "xpmile CLAUDE.md §WebSocket hand-off mechanics: remove_handler "
           "uses get_handle() internally; clearing m_handle first leaves the "
           "fd in epoll and the reactor dispatches to a deleted WebConnection.";
}

TEST(HandoffOrdering, SipWsBranch_PublishHappensAfterRemove)
{
    const std::string src   = read_file(kWebserviceSrc);
    const std::string block = extract_upgrade_block(src, "is_sip_ws_upgrade");
    ASSERT_FALSE(block.empty());

    const auto remove_pos = block.find("remove_handler");
    // BrowserStream is the publish target for /sip-ws.
    const auto publish_pos = block.find("new BrowserStream");

    ASSERT_NE(std::string::npos, remove_pos);
    ASSERT_NE(std::string::npos, publish_pos);
    EXPECT_LT(remove_pos, publish_pos)
        << "BrowserStream must be constructed AFTER remove_handler so the "
           "old WebConnection is fully out of epoll before the new handler "
           "claims the same fd.";
}

// ─────────────────────────────────────────────────────────────────────────────
// /agent — Layer 3 hand-off into AgentStream / CloudTunnelEndpoint
// ─────────────────────────────────────────────────────────────────────────────

TEST(HandoffOrdering, AgentBranch_RemoveHandlerBeforeClearingHandle)
{
    const std::string src   = read_file(kWebserviceSrc);
    const std::string block = extract_upgrade_block(src, "is_agent_upgrade");
    ASSERT_FALSE(block.empty()) << "could not locate the /agent upgrade block";

    const auto remove_pos = block.find("remove_handler");
    const auto clear_pos  = block.find("m_handle = ACE_INVALID_HANDLE");

    ASSERT_NE(std::string::npos, remove_pos);
    ASSERT_NE(std::string::npos, clear_pos);
    EXPECT_LT(remove_pos, clear_pos);
}

TEST(HandoffOrdering, AgentBranch_PublishHappensAfterRemove)
{
    const std::string src   = read_file(kWebserviceSrc);
    const std::string block = extract_upgrade_block(src, "is_agent_upgrade");
    ASSERT_FALSE(block.empty());

    const auto remove_pos  = block.find("remove_handler");
    const auto publish_pos = block.find("new AgentStream");

    ASSERT_NE(std::string::npos, remove_pos);
    ASSERT_NE(std::string::npos, publish_pos);
    EXPECT_LT(remove_pos, publish_pos);
}

// ─────────────────────────────────────────────────────────────────────────────
// /ws/db — xpmile's original branch. Regression guard ensuring our copy of
// xpmile webservice.cpp doesn't drift on this invariant either.
// ─────────────────────────────────────────────────────────────────────────────

TEST(HandoffOrdering, WsDbBranch_RemoveHandlerBeforeClearingHandle)
{
    const std::string src   = read_file(kWebserviceSrc);
    const std::string block = extract_upgrade_block(src, "is_ws_upgrade");
    ASSERT_FALSE(block.empty()) << "could not locate the /ws/db upgrade block";

    const auto remove_pos = block.find("remove_handler");
    const auto clear_pos  = block.find("m_handle = ACE_INVALID_HANDLE");

    ASSERT_NE(std::string::npos, remove_pos);
    ASSERT_NE(std::string::npos, clear_pos);
    EXPECT_LT(remove_pos, clear_pos);
}

TEST(HandoffOrdering, WsDbBranch_PublishHappensAfterRemove)
{
    const std::string src   = read_file(kWebserviceSrc);
    const std::string block = extract_upgrade_block(src, "is_ws_upgrade");
    ASSERT_FALSE(block.empty());

    const auto remove_pos  = block.find("remove_handler");
    const auto publish_pos = block.find("on_agent_connected");

    ASSERT_NE(std::string::npos, remove_pos);
    ASSERT_NE(std::string::npos, publish_pos);
    EXPECT_LT(remove_pos, publish_pos);
}

// ─────────────────────────────────────────────────────────────────────────────
// All three branches clear the stream's internal handle BEFORE remove_handler
// (it's fine to do — `m_stream.set_handle(INVALID)` doesn't affect what
// `get_handle()` returns; that's based on the event handler's `m_handle`,
// not the stream's).
//
// The check is more about robustness than the get_handle() concern — but
// the documented ordering also has set_handle first. Make sure we follow it.
// ─────────────────────────────────────────────────────────────────────────────

TEST(HandoffOrdering, AllBranches_ClearStreamHandleBeforeRemoveHandler)
{
    const std::string src = read_file(kWebserviceSrc);
    ASSERT_FALSE(src.empty());

    for (const char *marker :
         {"is_sip_ws_upgrade", "is_agent_upgrade", "is_ws_upgrade"}) {
        const std::string block = extract_upgrade_block(src, marker);
        ASSERT_FALSE(block.empty()) << "could not find block for " << marker;

        const auto stream_pos = block.find(
            "m_stream.set_handle(ACE_INVALID_HANDLE)");
        const auto remove_pos = block.find("remove_handler");
        ASSERT_NE(std::string::npos, stream_pos)
            << "block for " << marker
            << " must call m_stream.set_handle(ACE_INVALID_HANDLE) so the "
               "wrapped socket doesn't try to close the fd we just gave away";
        ASSERT_NE(std::string::npos, remove_pos);
        EXPECT_LT(stream_pos, remove_pos)
            << "block for " << marker << ": set_handle(INVALID) on the stream "
               "must precede remove_handler. Doing it after is harmless "
               "today, but the documented order keeps the destructor clean "
               "if remove_handler ever became blocking or threw.";
    }
}

TEST(HandoffOrdering, AllBranches_HandedOffFlagSetBeforeReleasing)
{
    const std::string src = read_file(kWebserviceSrc);
    ASSERT_FALSE(src.empty());

    for (const char *marker :
         {"is_sip_ws_upgrade", "is_agent_upgrade", "is_ws_upgrade"}) {
        const std::string block = extract_upgrade_block(src, marker);
        ASSERT_FALSE(block.empty());

        const auto flag_pos   = block.find("m_handedOff = true");
        const auto remove_pos = block.find("remove_handler");
        ASSERT_NE(std::string::npos, flag_pos)
            << marker << ": m_handedOff = true must be set so the destructor "
               "doesn't try to close the raw fd we transferred";
        ASSERT_NE(std::string::npos, remove_pos);
        EXPECT_LT(flag_pos, remove_pos)
            << marker << ": m_handedOff must be set BEFORE remove_handler "
               "(which can trigger destruction via the connection pool)";
    }
}
