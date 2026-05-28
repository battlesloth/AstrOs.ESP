#include <gmock/gmock.h>
#include <gtest/gtest.h>

// The OtaReceiver accessor is a small, native-testable surface despite the
// rest of OtaReceiver being MIXED. The native tests cover the accessor's
// thread-safe getter/setter shape via a minimal stand-in class that mirrors
// the production discipline (mutex-protected std::string).
//
// Why a stand-in instead of testing OtaReceiver directly: OtaReceiver pulls
// in ESP-IDF / FreeRTOS / mbedtls headers and cannot link in [env:test].
// The accessor's logic is small enough that mirroring it under a native
// stand-in catches the same kinds of regressions (mutex discipline,
// copy-vs-move semantics, empty-by-default).

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    // Mirror of OtaReceiver's accessor surface. Full integration is verified
    // on hardware via the bench QA plan; this test pins the accessor's mutex
    // discipline and copy semantics.
    class LastFirmwarePathHolder
    {
    public:
        std::optional<std::string> get() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (path_.empty())
            {
                return std::nullopt;
            }
            return path_;
        }

        void set(const std::string &path)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            path_ = path;
        }

        void clear()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            path_.clear();
        }

    private:
        mutable std::mutex mutex_;
        std::string path_;
    };
} // namespace

TEST(LastFirmwarePathHolder, EmptyByDefault)
{
    LastFirmwarePathHolder holder;
    EXPECT_EQ(std::nullopt, holder.get());
}

TEST(LastFirmwarePathHolder, ReturnsSetValue)
{
    LastFirmwarePathHolder holder;
    holder.set("/sdcard/firmware/abcd1234.bin");

    auto got = holder.get();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ("/sdcard/firmware/abcd1234.bin", *got);
}

TEST(LastFirmwarePathHolder, OverwriteReturnsLatest)
{
    LastFirmwarePathHolder holder;
    holder.set("/sdcard/firmware/first.bin");
    holder.set("/sdcard/firmware/second.bin");

    auto got = holder.get();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ("/sdcard/firmware/second.bin", *got);
}

TEST(LastFirmwarePathHolder, ClearReturnsEmpty)
{
    LastFirmwarePathHolder holder;
    holder.set("/sdcard/firmware/abcd1234.bin");
    holder.clear();
    EXPECT_EQ(std::nullopt, holder.get());
}

#include "../../lib/OtaForwarder/include/OtaForwarderQueueMessage.h"
#include "AstrOsEspNowProtocol.hpp"

#include <cstring>

// Free-helper contract: producer mallocs; consumer (or producer on send
// failure) calls freeOtaForwarderMsg. Pointers nulled after free so an
// accidental double-free is a no-op.

TEST(OtaForwarderMsg, FreeDeployBeginReleasesAllOwnedPointers)
{
    queue_ota_forwarder_msg_t m;
    std::memset(&m, 0, sizeof(m));
    m.kind = OTA_FWD_DEPLOY_BEGIN;
    m.transferId = strdup("xfer-7");
    m.deploy.msgId = strdup("msg-3");
    m.deploy.orderList = strdup("body\x1E"
                                "core\x1E"
                                "dome"); // string-literal concat avoids hex-escape greediness

    ASSERT_NE(nullptr, m.transferId);
    ASSERT_NE(nullptr, m.deploy.msgId);
    ASSERT_NE(nullptr, m.deploy.orderList);

    freeOtaForwarderMsg(&m);

    EXPECT_EQ(nullptr, m.transferId);
    EXPECT_EQ(nullptr, m.deploy.msgId);
    EXPECT_EQ(nullptr, m.deploy.orderList);
}

TEST(OtaForwarderMsg, FreeAckNakKindsAreNoOpForInlineFields)
{
    // The ACK/NAK kinds carry only inline fields (xferId, seqs, etc.).
    // freeOtaForwarderMsg should be safe on them (transferId is unused
    // for these kinds and stays nullptr).
    queue_ota_forwarder_msg_t m;
    std::memset(&m, 0, sizeof(m));
    m.kind = OTA_FWD_DATA_ACK;
    m.data_ack.xferId = 7;
    m.data_ack.highestContiguousSeq = 100;
    m.data_ack.nextExpectedSeq = 101;
    m.data_ack.windowRemaining = 7;

    freeOtaForwarderMsg(&m); // must not crash, must not free random memory
    // Fields stay set — only the malloc'd pointer arms get zeroed.
    EXPECT_EQ(7, m.data_ack.xferId);
}

TEST(OtaForwarderMsg, FreeTickIsNoOp)
{
    queue_ota_forwarder_msg_t m;
    std::memset(&m, 0, sizeof(m));
    m.kind = OTA_FWD_TICK;
    freeOtaForwarderMsg(&m); // must not crash; no allocated members
}

TEST(OtaForwarderMsg, FreeNullPtrIsNoOp)
{
    freeOtaForwarderMsg(nullptr); // must not crash
}

TEST(OtaForwarderMsg, FreeIsIdempotent)
{
    queue_ota_forwarder_msg_t m;
    std::memset(&m, 0, sizeof(m));
    m.kind = OTA_FWD_DEPLOY_BEGIN;
    m.transferId = strdup("x");
    m.deploy.msgId = strdup("m");
    m.deploy.orderList = strdup("o");

    freeOtaForwarderMsg(&m); // first free
    freeOtaForwarderMsg(&m); // second free — no-op because pointers nulled
}

// ─── AWAITING_VERSION_CONFIRMED stand-in ─────────────────────────────────────
//
// OtaForwarder is MIXED (lib_ignore'd in [env:test]) so the Phase A state
// machine cannot be driven directly. The stand-in below mirrors the two
// production methods verbatim:
//
//   OtaForwarder::checkPeerVersionForCurrentPadawan()
//   OtaForwarder::handleVersionConfirmTimeout()
//
// A settable peerVersions_ map stands in for AstrOs_EspNow.getPeerVersion().
// The result vector mimics OtaForwarder::results_ (PadawanResult shape).
// Phase is an enum class that mirrors the production Phase values exactly.
// Timer expiry is simulated by calling handleVersionConfirmTimeout() directly.
//
// The stand-in is intentionally minimal: only what is needed to exercise the
// three Phase A behaviours (match, timeout, parse-failure no-op).

namespace
{
    enum class StandInPhase : uint8_t
    {
        AWAITING_VERSION_CONFIRMED = 5,
        IDLE = 0,
    };

    struct StandInResult
    {
        std::string controllerId;
        AstrOsEspNowProtocol::PadawanStatus status;
        std::string finalVersion;
        std::string errorOrEmpty;
    };

    // Mirror of OtaForwarder's AWAITING_VERSION_CONFIRMED logic.
    // No FreeRTOS / ESP-IDF dependencies; suitable for [env:test].
    class VersionConfirmStandIn
    {
    public:
        // Settable test controls (mirrors private fields that production code
        // populates from the staged .bin header and peer heartbeats).
        std::string expectedNewVersion_;
        std::string currentControllerId_;
        StandInPhase phase_ = StandInPhase::AWAITING_VERSION_CONFIRMED;
        std::vector<StandInResult> results_;

        // Fake AstrOs_EspNow.getPeerVersion() backing store.
        std::unordered_map<std::string, std::string> peerVersions_;

        // Mirrors OtaForwarder::checkPeerVersionForCurrentPadawan().
        void checkPeerVersion()
        {
            if (phase_ != StandInPhase::AWAITING_VERSION_CONFIRMED)
                return;
            if (expectedNewVersion_.empty())
                return; // no comparison possible; fall through to timeout

            auto it = peerVersions_.find(currentControllerId_);
            std::string reported = (it != peerVersions_.end()) ? it->second : "";
            if (reported.empty() || reported != expectedNewVersion_)
                return; // still on old version; keep waiting

            // Version confirmed.
            phase_ = StandInPhase::IDLE;
            results_.push_back(
                {currentControllerId_, AstrOsEspNowProtocol::PadawanStatus::OK, expectedNewVersion_, ""});
        }

        // Mirrors OtaForwarder::handleVersionConfirmTimeout().
        void fireVersionConfirmTimeout()
        {
            if (phase_ != StandInPhase::AWAITING_VERSION_CONFIRMED)
                return; // stale fire
            results_.push_back(
                {currentControllerId_, AstrOsEspNowProtocol::PadawanStatus::FAILED, "", "version_unconfirmed"});
            phase_ = StandInPhase::IDLE;
        }

        // Mirrors the production sequence in handleFlashResult OK path:
        // clears the cached peer version before arming AWAITING_VERSION_CONFIRMED
        // so a pre-flash POLL_ACK can't false-match on the first tick.
        void enterAwaitingVersionConfirmed()
        {
            peerVersions_.erase(currentControllerId_);
            phase_ = StandInPhase::AWAITING_VERSION_CONFIRMED;
        }

        // Convenience setters used by test bodies.
        void setMockedPeerVersion(const std::string &mac, const std::string &version)
        {
            peerVersions_[mac] = version;
        }
        void setExpectedNewVersion(const std::string &version)
        {
            expectedNewVersion_ = version;
        }
        StandInPhase phase() const
        {
            return phase_;
        }
        const std::vector<StandInResult> &results() const
        {
            return results_;
        }
    };
} // namespace

// ─── Phase A state-machine tests ─────────────────────────────────────────────

TEST(VersionConfirmStandIn, HappyPath_MatchAdvancesToOK)
{
    VersionConfirmStandIn sm;
    sm.currentControllerId_ = "AA:BB:CC:DD:EE:01";
    sm.expectedNewVersion_ = "1.2.3";
    sm.peerVersions_["AA:BB:CC:DD:EE:01"] = "1.2.3";

    sm.checkPeerVersion(); // version matches on first tick

    EXPECT_EQ(sm.phase_, StandInPhase::IDLE);
    ASSERT_EQ(sm.results_.size(), 1u);
    EXPECT_EQ(sm.results_[0].status, AstrOsEspNowProtocol::PadawanStatus::OK);
    EXPECT_EQ(sm.results_[0].finalVersion, "1.2.3");
    EXPECT_EQ(sm.results_[0].errorOrEmpty, "");
}

TEST(VersionConfirmStandIn, Timeout_NoMatchRecordsFailed)
{
    VersionConfirmStandIn sm;
    sm.currentControllerId_ = "AA:BB:CC:DD:EE:02";
    sm.expectedNewVersion_ = "1.2.3";
    sm.peerVersions_["AA:BB:CC:DD:EE:02"] = "1.1.0"; // stuck on old version

    // Simulate 15 ticks with no match.
    for (int i = 0; i < 15; ++i)
        sm.checkPeerVersion();

    // Still waiting.
    EXPECT_EQ(sm.phase_, StandInPhase::AWAITING_VERSION_CONFIRMED);
    EXPECT_TRUE(sm.results_.empty());

    // 15 s timer fires.
    sm.fireVersionConfirmTimeout();

    ASSERT_EQ(sm.results_.size(), 1u);
    EXPECT_EQ(sm.results_[0].status, AstrOsEspNowProtocol::PadawanStatus::FAILED);
    EXPECT_EQ(sm.results_[0].errorOrEmpty, "version_unconfirmed");
    EXPECT_EQ(sm.results_[0].finalVersion, "");
}

TEST(VersionConfirmStandIn, ParseFailure_EmptyExpectedIsNoOpUntilTimeout)
{
    VersionConfirmStandIn sm;
    sm.currentControllerId_ = "AA:BB:CC:DD:EE:03";
    sm.expectedNewVersion_ = "";                     // simulates failed esp_app_desc parse at deploy start
    sm.peerVersions_["AA:BB:CC:DD:EE:03"] = "1.2.3"; // peer has a version, but no comparison is possible

    // Tick handler must be a no-op — no match attempted, no result recorded.
    for (int i = 0; i < 15; ++i)
        sm.checkPeerVersion();

    EXPECT_EQ(sm.phase_, StandInPhase::AWAITING_VERSION_CONFIRMED);
    EXPECT_TRUE(sm.results_.empty());

    // Timer fires and records FAILED (same path as timeout).
    sm.fireVersionConfirmTimeout();

    ASSERT_EQ(sm.results_.size(), 1u);
    EXPECT_EQ(sm.results_[0].status, AstrOsEspNowProtocol::PadawanStatus::FAILED);
    EXPECT_EQ(sm.results_[0].errorOrEmpty, "version_unconfirmed");
}

TEST(VersionConfirmStandIn, SameVersionDeploy_DoesNotFalseMatchOnPreFlashCachedVersion)
{
    // Simulate same-version re-deploy: cache pre-populated with v1.2.3,
    // expected new version also v1.2.3, padawan hasn't yet sent a fresh
    // POLL_ACK after the arm.
    static const char *kMac = "AA:BB:CC:DD:EE:04";

    VersionConfirmStandIn standIn;
    standIn.currentControllerId_ = kMac;
    standIn.setMockedPeerVersion(kMac, "1.2.3");
    standIn.setExpectedNewVersion("1.2.3");
    standIn.enterAwaitingVersionConfirmed(); // production now calls clearPeerVersion here

    // First tick should NOT match — cache should have been cleared.
    standIn.checkPeerVersion();
    EXPECT_EQ(standIn.phase(), StandInPhase::AWAITING_VERSION_CONFIRMED);
    EXPECT_EQ(standIn.results().size(), 0u);

    // Now simulate a post-arm POLL_ACK landing with the same version (genuine
    // post-reboot heartbeat).
    standIn.setMockedPeerVersion(kMac, "1.2.3");

    // Next tick should match and record OK.
    standIn.checkPeerVersion();
    EXPECT_EQ(standIn.results().size(), 1u);
    EXPECT_EQ(standIn.results()[0].status, AstrOsEspNowProtocol::PadawanStatus::OK);
    EXPECT_EQ(standIn.results()[0].finalVersion, "1.2.3");
}
