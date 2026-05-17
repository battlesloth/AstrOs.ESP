#include <OtaReceiver.hpp>

#include <AstrOsSerialMsgHandler.hpp>
#include <AstrOsStorageManager.hpp>
#include <AstrOsStringUtils.hpp>
#include <esp_log.h>

#include <cstdio>
#include <cstring>
#include <unistd.h>

static const char *TAG = "OtaReceiver";

namespace
{
    // Lowercase-hex encoding of `len` bytes from `in` into `out`. `out` must
    // hold at least 2*len + 1 chars; this writes the NUL.
    void toHexLower(const uint8_t *in, size_t len, char *out)
    {
        static const char kHex[] = "0123456789abcdef";
        for (size_t i = 0; i < len; ++i)
        {
            out[2 * i] = kHex[(in[i] >> 4) & 0x0F];
            out[2 * i + 1] = kHex[in[i] & 0x0F];
        }
        out[2 * len] = '\0';
    }

    constexpr const char *kStagingPath = "/sdcard/firmware/staging.bin";
} // namespace

OtaReceiver AstrOs_OtaReceiver;

OtaReceiver::OtaReceiver() {}

OtaReceiver::~OtaReceiver()
{
    // The singleton is process-lifetime in firmware; this dtor exists for
    // native-link symmetry. esp_timer_stop on an idle timer is harmless.
    if (watchdog_ != nullptr)
    {
        esp_timer_stop(watchdog_);
        esp_timer_delete(watchdog_);
        watchdog_ = nullptr;
    }
    // Shutdown is not a transfer outcome — preserve any in-flight staging.bin
    // for the next boot rather than deleting it.
    resetCryptoAndFile(/*keepStaging=*/true);
}

void OtaReceiver::Init(QueueHandle_t otaQueue)
{
    // Idempotent: a second Init() would leak the first esp_timer handle.
    if (watchdog_ != nullptr)
    {
        ESP_LOGW(TAG, "Init() called twice — ignoring second call");
        return;
    }

    otaQueue_ = otaQueue;

    const esp_timer_create_args_t args = {
        .callback = &OtaReceiver::watchdogTimerCb,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ota_receiver_watchdog",
        .skip_unhandled_events = false,
    };
    esp_err_t err = esp_timer_create(&args, &watchdog_);
    if (err != ESP_OK)
    {
        // Receiver still serves happy-path transfers without the watchdog —
        // only stuck-recovery is lost.
        ESP_LOGE(TAG, "esp_timer_create(ota_receiver_watchdog) failed: %s — watchdog disabled", esp_err_to_name(err));
        watchdog_ = nullptr;
    }

    ESP_LOGI(TAG, "OtaReceiver initialized (watchdog idle threshold: %llums)", kWatchdogIdleUs / 1000ULL);
}

void OtaReceiver::watchdogTimerCb(void *arg)
{
    auto self = static_cast<OtaReceiver *>(arg);
    if (self->otaQueue_ == nullptr)
    {
        return;
    }
    queue_ota_msg_t msg{};
    msg.kind = OTA_MSG_WATCHDOG_FIRE;
    msg.transferId = nullptr;
    // Non-blocking — a full queue means otaReceiverTask is already wedged;
    // dropping the signal is the least-bad outcome.
    if (xQueueSend(self->otaQueue_, &msg, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "watchdog: otaQueue full, dropping WATCHDOG_FIRE signal");
    }
}

void OtaReceiver::watchdogStart()
{
    if (watchdog_ == nullptr)
        return;
    esp_err_t err = esp_timer_start_once(watchdog_, kWatchdogIdleUs);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "watchdog: esp_timer_start_once failed: %s", esp_err_to_name(err));
    }
}

void OtaReceiver::watchdogRestart()
{
    if (watchdog_ == nullptr)
        return;
    // Stop-then-start: esp_timer has no native restart. Stop on an idle timer
    // is a harmless no-op.
    esp_timer_stop(watchdog_);
    esp_err_t err = esp_timer_start_once(watchdog_, kWatchdogIdleUs);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "watchdog: esp_timer_start_once (restart) failed: %s — transfer no longer protected",
                 esp_err_to_name(err));
    }
}

void OtaReceiver::watchdogStop()
{
    if (watchdog_ == nullptr)
        return;
    esp_timer_stop(watchdog_);
}

void OtaReceiver::resetCryptoAndFile(bool keepStaging)
{
    if (staging_ != nullptr)
    {
        fclose(staging_);
        staging_ = nullptr;
        if (!keepStaging)
        {
            // unlink may fail if the file was already renamed (END OK path);
            // ignore — the goal is to leave the path absent.
            unlink(kStagingPath);
        }
    }
    if (shaActive_)
    {
        mbedtls_sha256_free(&shaCtx_);
        shaActive_ = false;
    }
}

void OtaReceiver::process(queue_ota_msg_t &msg)
{
    switch (msg.kind)
    {
    case OTA_MSG_BEGIN:
        handleBegin(msg);
        break;
    case OTA_MSG_CHUNK:
        handleChunk(msg);
        break;
    case OTA_MSG_END:
        handleEnd(msg);
        break;
    case OTA_MSG_DEPLOY_BEGIN:
        handleDeployBegin(msg);
        break;
    case OTA_MSG_WATCHDOG_FIRE:
        handleWatchdogFire();
        break;
    default:
        // Unknown kind surfaces the missed enumerator without bricking the
        // box. freeOtaMsg below will free only transferId for unknown kinds.
        ESP_LOGE(TAG, "Unknown ota_msg_kind_t: %d transferId=%s — dropping", static_cast<int>(msg.kind),
                 msg.transferId ? msg.transferId : "(null)");
        break;
    }
    freeOtaMsg(&msg);
}

void OtaReceiver::handleWatchdogFire()
{
    if (!active_)
    {
        // Stale fire — a prior end/watchdog already cleared state but the
        // signal was already in flight. Idempotent no-op.
        return;
    }
    ESP_LOGW(TAG, "OTA watchdog fired: transferId=%s idle >%llums — aborting transfer", transferIdStr_.c_str(),
             kWatchdogIdleUs / 1000ULL);
    bulk_.reset();
    // Unverified partial bytes — no END means no hash check, so discarding
    // staging.bin is correct here (different policy from HASH_MISMATCH, which
    // preserves for forensics).
    resetCryptoAndFile(/*keepStaging=*/false);
    active_ = false;
    transferIdStr_.clear();
    // No server notification — chunk_streamer has its own timeout. The
    // master's job is to be ready for the next BEGIN.
}

void OtaReceiver::handleBegin(queue_ota_msg_t &msg)
{
    std::string msgId = msg.begin.msgId ? msg.begin.msgId : "";
    std::string transferIdIn = msg.transferId ? msg.transferId : "";

    if (active_)
    {
        // Don't touch staging_/shaCtx_ — they belong to the in-flight transfer.
        ESP_LOGW(TAG, "FW_TRANSFER_BEGIN while transfer %s active; replying busy", transferIdStr_.c_str());
        AstrOs_SerialMsgHandler.sendFwTransferBeginAck(msgId, transferIdIn, "busy");
        return;
    }

    // BulkReceiver wants a uint8_t; parseStrictU8 rejects whitespace and signed forms.
    auto parsed = AstrOsStringUtils::parseStrictU8(transferIdIn);
    if (!parsed.has_value())
    {
        ESP_LOGE(TAG, "FW_TRANSFER_BEGIN transferId='%s' is not 0..255 numeric", transferIdIn.c_str());
        AstrOs_SerialMsgHandler.sendFwTransferBeginAck(msgId, transferIdIn, "io_error");
        return;
    }
    uint8_t xferId = parsed.value();

    // Matches the server's nominal sender window.
    constexpr uint8_t kWindowSize = 16;

    auto br = bulk_.begin(xferId, msg.begin.totalSize, msg.begin.totalChunks, msg.begin.chunkSize, kWindowSize);
    if (!br.valid)
    {
        ESP_LOGW(TAG, "BulkReceiver::begin rejected: reason=%d (totalSize=%u chunks=%u chunkSize=%u)",
                 static_cast<int>(br.reason), (unsigned)msg.begin.totalSize, (unsigned)msg.begin.totalChunks,
                 (unsigned)msg.begin.chunkSize);
        AstrOs_SerialMsgHandler.sendFwTransferBeginAck(msgId, transferIdIn, "io_error");
        return;
    }

    // SD-staging gate. Order matters: ensureSdFirmwareDir, then free-space
    // pre-check, then open. Any failure must NOT mutate active_/transferIdStr_
    // so a rejected BEGIN leaves the receiver exactly as it was.
    if (!AstrOs_Storage.ensureSdFirmwareDir())
    {
        ESP_LOGE(TAG, "FW_TRANSFER_BEGIN: ensureSdFirmwareDir failed — replying io_error");
        bulk_.reset();
        AstrOs_SerialMsgHandler.sendFwTransferBeginAck(msgId, transferIdIn, "io_error");
        return;
    }

    uint64_t freeBytes = AstrOs_Storage.freeSpaceSdBytes();
    if (freeBytes < msg.begin.totalSize)
    {
        ESP_LOGW(TAG, "FW_TRANSFER_BEGIN: sd_full (free=%llu need=%u)", (unsigned long long)freeBytes,
                 (unsigned)msg.begin.totalSize);
        bulk_.reset();
        AstrOs_SerialMsgHandler.sendFwTransferBeginAck(msgId, transferIdIn, "sd_full");
        return;
    }

    // "wb" truncates — a HASH_MISMATCH leftover from the prior transfer is
    // replaced cleanly here without a separate "previous-transfer cleanup" step.
    staging_ = fopen(kStagingPath, "wb");
    if (staging_ == nullptr)
    {
        ESP_LOGE(TAG, "FW_TRANSFER_BEGIN: fopen(%s) failed: errno=%d (%s)", kStagingPath, errno, strerror(errno));
        bulk_.reset();
        AstrOs_SerialMsgHandler.sendFwTransferBeginAck(msgId, transferIdIn, "io_error");
        return;
    }

    mbedtls_sha256_init(&shaCtx_);
    if (mbedtls_sha256_starts(&shaCtx_, /*is224=*/0) != 0)
    {
        ESP_LOGE(TAG, "FW_TRANSFER_BEGIN: mbedtls_sha256_starts failed");
        mbedtls_sha256_free(&shaCtx_);
        fclose(staging_);
        staging_ = nullptr;
        unlink(kStagingPath);
        bulk_.reset();
        AstrOs_SerialMsgHandler.sendFwTransferBeginAck(msgId, transferIdIn, "io_error");
        return;
    }
    shaActive_ = true;

    active_ = true;
    transferIdStr_ = transferIdIn;

    ESP_LOGI(TAG, "FW_TRANSFER_BEGIN accepted: transferId=%s totalSize=%u chunks=%u sha=%s", transferIdIn.c_str(),
             (unsigned)msg.begin.totalSize, (unsigned)msg.begin.totalChunks, msg.begin.sha256Hex);

    AstrOs_SerialMsgHandler.sendFwTransferBeginAck(msgId, transferIdIn, "OK");
    watchdogStart();
}

void OtaReceiver::handleChunk(queue_ota_msg_t &msg)
{
    std::string transferIdIn = msg.transferId ? msg.transferId : "";

    if (!active_)
    {
        ESP_LOGW(TAG, "FW_CHUNK while no transfer active; emitting inactive NAK");
        AstrOs_SerialMsgHandler.sendFwChunkNak(transferIdIn, /*lastGoodSeq=*/0, /*nextExpectedSeq=*/0, "OUT_OF_ORDER");
        return;
    }

    auto parsedChunk = AstrOsStringUtils::parseStrictU8(transferIdIn);
    if (!parsedChunk.has_value())
    {
        ESP_LOGW(TAG, "FW_CHUNK transferId='%s' not numeric; emitting OUT_OF_ORDER", transferIdIn.c_str());
        AstrOs_SerialMsgHandler.sendFwChunkNak(transferIdIn, /*lastGoodSeq=*/0, /*nextExpectedSeq=*/0, "OUT_OF_ORDER");
        return;
    }
    uint8_t xferId = parsedChunk.value();

    auto cr = bulk_.onChunk(xferId, msg.chunk.seq, msg.chunk.payloadLen, msg.chunk.crc16, msg.chunk.payload);

    if (cr.decision == AstrOsBulkTransport::Decision::ACK)
    {
        // Persist before ACK'ing. A short fwrite (full disk, fs error) means
        // the byte stream we'd be ACK'ing is incomplete, so we NAK with
        // FLASH_FULL and tear the transfer down — the server's next BEGIN
        // truncates staging.bin and starts over.
        if (staging_ == nullptr || !shaActive_)
        {
            // Defensive: should be impossible if handleBegin succeeded. Treat
            // as terminal so the next BEGIN can recover.
            ESP_LOGE(TAG, "handleChunk ACK but staging/sha not initialized — aborting transfer");
            AstrOs_SerialMsgHandler.sendFwChunkNak(transferIdIn, cr.highestContiguousSeq, cr.nextExpectedSeq,
                                                   "FLASH_FULL");
            resetCryptoAndFile(/*keepStaging=*/false);
            bulk_.reset();
            active_ = false;
            transferIdStr_.clear();
            watchdogStop();
            return;
        }
        size_t wrote = fwrite(cr.payload, 1, cr.payloadLen, staging_);
        if (wrote != cr.payloadLen)
        {
            ESP_LOGE(TAG, "fwrite short: wrote=%zu of %u (errno=%d %s) — aborting transfer", wrote,
                     (unsigned)cr.payloadLen, errno, strerror(errno));
            AstrOs_SerialMsgHandler.sendFwChunkNak(transferIdIn, cr.highestContiguousSeq, cr.nextExpectedSeq,
                                                   "FLASH_FULL");
            resetCryptoAndFile(/*keepStaging=*/false);
            bulk_.reset();
            active_ = false;
            transferIdStr_.clear();
            watchdogStop();
            return;
        }
        if (mbedtls_sha256_update(&shaCtx_, cr.payload, cr.payloadLen) != 0)
        {
            // Hash failure is rare but unrecoverable — END would compare a
            // corrupt digest and reply HASH_MISMATCH anyway. Fail fast.
            ESP_LOGE(TAG, "mbedtls_sha256_update failed — aborting transfer");
            AstrOs_SerialMsgHandler.sendFwChunkNak(transferIdIn, cr.highestContiguousSeq, cr.nextExpectedSeq,
                                                   "FLASH_FULL");
            resetCryptoAndFile(/*keepStaging=*/false);
            bulk_.reset();
            active_ = false;
            transferIdStr_.clear();
            watchdogStop();
            return;
        }
        AstrOs_SerialMsgHandler.sendFwChunkAck(transferIdIn, cr.highestContiguousSeq, cr.nextExpectedSeq,
                                               cr.windowRemaining);
    }
    else
    {
        const char *reasonStr = "OUT_OF_ORDER";
        switch (cr.reason)
        {
        case AstrOsBulkTransport::NakReason::CRC:
            reasonStr = "CRC";
            break;
        case AstrOsBulkTransport::NakReason::SIZE:
            reasonStr = "SIZE";
            break;
        case AstrOsBulkTransport::NakReason::OUT_OF_ORDER:
            reasonStr = "OUT_OF_ORDER";
            break;
        case AstrOsBulkTransport::NakReason::FLASH_FULL:
            reasonStr = "FLASH_FULL";
            break;
        case AstrOsBulkTransport::NakReason::NONE:
            // NONE on the NAK path is unreachable; force CRC for safe retransmit.
            ESP_LOGE(TAG, "BulkReceiver returned NAK with reason=NONE; forcing CRC");
            reasonStr = "CRC";
            break;
        default:
            ESP_LOGE(TAG, "Unknown NakReason value %d — forcing CRC", static_cast<int>(cr.reason));
            reasonStr = "CRC";
            break;
        }
        AstrOs_SerialMsgHandler.sendFwChunkNak(transferIdIn, cr.highestContiguousSeq, cr.nextExpectedSeq, reasonStr);
    }

    // The watchdog protects against silence, not slow progress — any chunk
    // activity (ACK or NAK) resets it. chunk_streamer handles retry exhaustion.
    watchdogRestart();
}

void OtaReceiver::handleEnd(queue_ota_msg_t &msg)
{
    std::string transferIdIn = msg.transferId ? msg.transferId : "";
    std::string msgId = msg.end.msgId ? msg.end.msgId : "";
    std::string finalShaIn(msg.end.finalSha256Hex);

    auto parsedEnd = AstrOsStringUtils::parseStrictU8(transferIdIn);
    if (!parsedEnd.has_value())
    {
        // Unparseable transferId — can't tell if this END is for our transfer.
        // NAK without touching state so a malformed END can't clobber it.
        ESP_LOGW(TAG, "FW_TRANSFER_END transferId='%s' not numeric; emitting IO_ERROR (state preserved)",
                 transferIdIn.c_str());
        AstrOs_SerialMsgHandler.sendFwTransferEndAck(msgId, transferIdIn, "IO_ERROR", finalShaIn);
        return;
    }
    uint8_t xferId = parsedEnd.value();

    auto er = bulk_.onEnd(xferId, msg.end.totalChunks);

    if (er.status == AstrOsBulkTransport::EndResult::Status::OK)
    {
        // Drain stdio buffers before finishing the hash. Without this, bytes
        // queued inside the FILE* would never reach disk and a power-loss
        // immediately after END_ACK OK could leave the renamed file short.
        bool closeOk = (staging_ != nullptr) && (fclose(staging_) == 0);
        staging_ = nullptr;

        uint8_t digest[32];
        bool hashOk = shaActive_ && (mbedtls_sha256_finish(&shaCtx_, digest) == 0);
        if (shaActive_)
        {
            mbedtls_sha256_free(&shaCtx_);
            shaActive_ = false;
        }

        if (!closeOk || !hashOk)
        {
            ESP_LOGE(TAG, "FW_TRANSFER_END finalize failed (closeOk=%d hashOk=%d) — replying IO_ERROR", (int)closeOk,
                     (int)hashOk);
            // Keep staging.bin so a follow-up can inspect what was written.
            AstrOs_SerialMsgHandler.sendFwTransferEndAck(msgId, transferIdIn, "IO_ERROR", "");
        }
        else
        {
            char computedHex[SHA256_HEX_LEN + 1];
            toHexLower(digest, sizeof(digest), computedHex);

            const char *expectedHex = msg.end.finalSha256Hex;
            if (strcmp(computedHex, expectedHex) == 0)
            {
                // Content-addressed final name: 16-hex-char prefix is enough
                // to disambiguate firmware images in practice and keeps the
                // path short. unlink-then-rename tolerates same-firmware
                // re-uploads (POSIX rename onto an existing path can fail on
                // FAT).
                char finalPath[64];
                snprintf(finalPath, sizeof(finalPath), "/sdcard/firmware/%.16s.bin", computedHex);
                unlink(finalPath);
                if (rename(kStagingPath, finalPath) == 0)
                {
                    ESP_LOGI(TAG, "FW_TRANSFER_END OK: transferId=%s totalChunks=%u path=%s sha=%s",
                             transferIdIn.c_str(), (unsigned)msg.end.totalChunks, finalPath, computedHex);
                    AstrOs_SerialMsgHandler.sendFwTransferEndAck(msgId, transferIdIn, "OK", computedHex);
                }
                else
                {
                    // Verified bytes are on disk but couldn't be moved into
                    // place. Leave staging.bin so a follow-up phase can still
                    // consume them; signal the failure honestly.
                    ESP_LOGE(TAG, "FW_TRANSFER_END: rename(%s -> %s) failed: errno=%d (%s)", kStagingPath, finalPath,
                             errno, strerror(errno));
                    AstrOs_SerialMsgHandler.sendFwTransferEndAck(msgId, transferIdIn, "IO_ERROR", computedHex);
                }
            }
            else
            {
                ESP_LOGW(TAG,
                         "FW_TRANSFER_END HASH_MISMATCH: transferId=%s computed=%s expected=%s — staging.bin preserved",
                         transferIdIn.c_str(), computedHex, expectedHex);
                // Forensic preservation per design — do NOT unlink staging.bin.
                AstrOs_SerialMsgHandler.sendFwTransferEndAck(msgId, transferIdIn, "HASH_MISMATCH", computedHex);
            }
        }
    }
    else
    {
        // WRONG_XFER_ID and SENDER_TOTAL_MISMATCH are server-state bugs (LOGE);
        // other reasons stay at LOGW.
        const char *reasonStr = "(unknown)";
        bool serverBug = false;
        switch (er.reason)
        {
        case AstrOsBulkTransport::EndResult::Reason::NONE:
            reasonStr = "NONE";
            break;
        case AstrOsBulkTransport::EndResult::Reason::NOT_ACTIVE:
            reasonStr = "NOT_ACTIVE";
            break;
        case AstrOsBulkTransport::EndResult::Reason::WRONG_XFER_ID:
            reasonStr = "WRONG_XFER_ID";
            serverBug = true;
            break;
        case AstrOsBulkTransport::EndResult::Reason::SENDER_TOTAL_MISMATCH:
            reasonStr = "SENDER_TOTAL_MISMATCH";
            serverBug = true;
            break;
        case AstrOsBulkTransport::EndResult::Reason::RECEIVER_SHORT_COUNT:
            reasonStr = "RECEIVER_SHORT_COUNT";
            break;
        default:
            ESP_LOGE(TAG, "Unknown EndResult::Reason value %d", static_cast<int>(er.reason));
            serverBug = true;
            break;
        }
        if (serverBug)
        {
            ESP_LOGE(TAG, "FW_TRANSFER_END IO_ERROR reason=%s (transferId=%s server-state bug)", reasonStr,
                     transferIdIn.c_str());
        }
        else
        {
            ESP_LOGW(TAG, "FW_TRANSFER_END IO_ERROR reason=%s (transferId=%s)", reasonStr, transferIdIn.c_str());
        }
        AstrOs_SerialMsgHandler.sendFwTransferEndAck(msgId, transferIdIn, "IO_ERROR", finalShaIn);
    }

    // Tear down only when the END applied to our running transfer — see
    // shouldTeardownOnEndResult's docstring for the teardown vs preserve set.
    if (AstrOsBulkTransport::shouldTeardownOnEndResult(er))
    {
        active_ = false;
        transferIdStr_.clear();
        bulk_.reset();
        watchdogStop();
        // HASH_MISMATCH preserves staging.bin (handled inline above by not
        // unlinking and leaving staging_ closed). Every other teardown path —
        // RECEIVER_SHORT_COUNT, finalize-failure IO_ERROR, OK rename success
        // — has already closed the FILE* but the helper is idempotent and
        // also covers the case where the OK branch above didn't run (e.g.,
        // server-state bugs reaching the IO_ERROR branch with staging_ still
        // open). keepStaging=true is the conservative choice: forensic
        // inspection beats silent deletion.
        resetCryptoAndFile(/*keepStaging=*/true);
    }
}

void OtaReceiver::handleDeployBegin(queue_ota_msg_t &msg)
{
    std::string transferIdIn = msg.transferId ? msg.transferId : "";
    std::string msgId = msg.deploy.msgId ? msg.deploy.msgId : "";
    std::string orderListStr = msg.deploy.orderList ? msg.deploy.orderList : "";

    if (orderListStr.empty())
    {
        // sendFwDeployDone would drop empty results — skip to avoid double-log.
        ESP_LOGE(TAG, "FW_DEPLOY_BEGIN orderList empty — dropping");
        return;
    }

    std::vector<astros_fw_deploy_result_t> results;
    size_t start = 0;
    while (start < orderListStr.size())
    {
        size_t end = orderListStr.find('\x1E', start);
        std::string id =
            (end == std::string::npos) ? orderListStr.substr(start) : orderListStr.substr(start, end - start);
        if (!id.empty())
        {
            results.push_back({id, "FAILED", "", "not_implemented"});
        }
        if (end == std::string::npos)
        {
            break;
        }
        start = end + 1;
    }

    ESP_LOGI(TAG, "FW_DEPLOY_BEGIN: transferId=%s target-count=%zu — all-FAILED not_implemented", transferIdIn.c_str(),
             results.size());

    AstrOs_SerialMsgHandler.sendFwDeployDone(msgId, transferIdIn, results);
}
