#include <OtaWriter.hpp>

#include <AstrOsEspNowService.hpp>
#include <esp_log.h>

#include <algorithm>
#include <cstring>

static const char *TAG = "OtaWriter";

OtaWriter AstrOs_OtaWriter;

OtaWriter::OtaWriter() {}

OtaWriter::~OtaWriter()
{
    // esp_timer_stop on an idle timer is harmless.
    for (esp_timer_handle_t *t : {&watchdog_, &statsTimer_})
    {
        if (*t != nullptr)
        {
            esp_timer_stop(*t);
            esp_timer_delete(*t);
            *t = nullptr;
        }
    }
    resetOtaHandleAndSha();
}

void OtaWriter::Init(QueueHandle_t otaWriterQueue)
{
    // Idempotent: a second Init() would leak the first esp_timer handle.
    if (watchdog_ != nullptr)
    {
        ESP_LOGW(TAG, "Init() called twice — ignoring second call");
        return;
    }

    otaWriterQueue_ = otaWriterQueue;

    const esp_timer_create_args_t args = {
        .callback = &OtaWriter::watchdogTimerCb,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ota_writer_watchdog",
        .skip_unhandled_events = false,
    };
    esp_err_t err = esp_timer_create(&args, &watchdog_);
    if (err != ESP_OK)
    {
        // Writer still serves happy-path transfers without the watchdog —
        // only stuck-recovery is lost. Mirrors OtaReceiver's choice.
        ESP_LOGE(TAG, "esp_timer_create(ota_writer_watchdog) failed: %s — watchdog disabled", esp_err_to_name(err));
        watchdog_ = nullptr;
    }

    const esp_timer_create_args_t statsArgs = {
        .callback = &OtaWriter::statsTimerCb,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ota_writer_stats",
        .skip_unhandled_events = true,
    };
    esp_err_t statsErr = esp_timer_create(&statsArgs, &statsTimer_);
    if (statsErr != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_timer_create(ota_writer_stats) failed: %s — stats log disabled", esp_err_to_name(statsErr));
        statsTimer_ = nullptr;
    }

    ESP_LOGI(TAG, "OtaWriter initialized (watchdog idle threshold: %llums)", kWatchdogIdleUs / 1000ULL);
}

void OtaWriter::watchdogTimerCb(void *arg)
{
    auto self = static_cast<OtaWriter *>(arg);
    if (self->otaWriterQueue_ == nullptr)
    {
        return;
    }
    queue_ota_writer_msg_t msg{};
    msg.kind = OTA_WR_WATCHDOG_FIRE;
    // Non-blocking — a full queue means otaWriterTask is already wedged;
    // dropping the signal is the least-bad outcome.
    if (xQueueSend(self->otaWriterQueue_, &msg, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "watchdog: otaWriterQueue full, dropping WATCHDOG_FIRE signal");
    }
}

void OtaWriter::watchdogStart()
{
    if (watchdog_ == nullptr)
        return;
    esp_err_t err = esp_timer_start_once(watchdog_, kWatchdogIdleUs);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "watchdog: esp_timer_start_once failed: %s", esp_err_to_name(err));
    }
}

void OtaWriter::watchdogRestart()
{
    if (watchdog_ == nullptr)
        return;
    // esp_timer's one-shot has no native restart; stop-then-start.
    // esp_timer_stop on a non-running timer returns ESP_ERR_INVALID_STATE — benign.
    esp_timer_stop(watchdog_);
    esp_err_t err = esp_timer_start_once(watchdog_, kWatchdogIdleUs);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "watchdog: restart esp_timer_start_once failed: %s", esp_err_to_name(err));
    }
}

void OtaWriter::watchdogStop()
{
    if (watchdog_ == nullptr)
        return;
    esp_timer_stop(watchdog_);
}

void OtaWriter::statsTimerCb(void *arg)
{
    auto self = static_cast<OtaWriter *>(arg);
    if (self->otaWriterQueue_ == nullptr)
    {
        return;
    }
    queue_ota_writer_msg_t msg{};
    msg.kind = OTA_WR_STATS_FIRE;
    // Best-effort: missing one stats fire is harmless; next tick catches up.
    xQueueSend(self->otaWriterQueue_, &msg, 0);
}

void OtaWriter::statsTimerStart()
{
    if (statsTimer_ == nullptr)
        return;
    esp_timer_stop(statsTimer_);
    esp_err_t err = esp_timer_start_periodic(statsTimer_, kStatsPeriodUs);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "stats: esp_timer_start_periodic failed: %s — stats log will be silent", esp_err_to_name(err));
    }
}

void OtaWriter::statsTimerStop()
{
    if (statsTimer_ == nullptr)
        return;
    esp_timer_stop(statsTimer_);
}

void OtaWriter::handleStatsFire()
{
    if (!active_)
    {
        // Stale fire after transfer ended; stop the timer to silence further
        // noise (start/stop wiring guarantees this is rare).
        statsTimerStop();
        return;
    }
    const long long acked = statsAnyAcked_ ? static_cast<long long>(statsHighestAckedSeq_) : -1;
    ESP_LOGI(TAG, "OTA_STATS_RX: xferId=%u seq=%u/%u acked=%lld naks-tx(CRC=%u SIZE=%u OOO=%u FLASH=%u) send-fail=%u",
             (unsigned)currentXferId_, (unsigned)statsLastRecvSeq_, (unsigned)currentTotalChunks_, acked,
             (unsigned)statsNaksCRC_, (unsigned)statsNaksSIZE_, (unsigned)statsNaksOOO_, (unsigned)statsNaksFLASH_,
             (unsigned)statsSendFailCount_);
}

void OtaWriter::resetOtaHandleAndSha()
{
    // Stop the watchdog first so a pending fire either no-ops on the
    // already-stopped timer or queues a WATCHDOG_FIRE that hits the
    // late-signal path below (active_ = false by the time it dispatches).
    watchdogStop();
    statsTimerStop();
    if (otaHandle_ != 0)
    {
        // esp_ota_abort releases the handle. Return value isn't actionable
        // here — we're already on the abort path.
        esp_err_t err = esp_ota_abort(otaHandle_);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "esp_ota_abort returned %s — partition may be in inconsistent state", esp_err_to_name(err));
        }
        otaHandle_ = 0;
    }
    // AstrOsSha256 owns no heap; just drop the active flag.
    shaActive_ = false;
    bulk_.reset();
    inactivePartition_ = nullptr;
    currentXferId_ = 0;
    memset(currentMasterMac_, 0, sizeof(currentMasterMac_));
    currentTotalSize_ = 0;
    memset(expectedSha256_, 0, sizeof(expectedSha256_));
    active_ = false;
}

void OtaWriter::logSendResult(const char *site, esp_err_t err)
{
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "%s send failed: %s — master timeout will recover", site, esp_err_to_name(err));
    }
}

void OtaWriter::process(queue_ota_writer_msg_t &msg)
{
    switch (msg.kind)
    {
    case OTA_WR_BEGIN:
        handleBegin(msg);
        break;
    case OTA_WR_DATA:
        handleData(msg);
        break;
    case OTA_WR_END:
        handleEnd(msg);
        break;
    case OTA_WR_WATCHDOG_FIRE:
        handleWatchdogFire();
        break;
    case OTA_WR_STATS_FIRE:
        handleStatsFire();
        break;
    default:
        ESP_LOGE(TAG, "process: unknown msg.kind=%d", (int)msg.kind);
        break;
    }
    // process() owns the free contract; caller must not free externally.
    freeOtaWriterMsg(&msg);
}

void OtaWriter::handleBegin(queue_ota_writer_msg_t &msg)
{
    const uint8_t *mac = msg.begin.srcMac;
    const uint8_t xferId = msg.begin.xferId;

    if (active_)
    {
        ESP_LOGW(TAG, "handleBegin: xferId=%u arrived while xferId=%u is active — replying BUSY", xferId,
                 currentXferId_);
        logSendResult("handleBegin BUSY NAK", sendBeginNak(mac, xferId, OtaBeginNakReason::BUSY));
        return;
    }

    // esp_ota_get_next_update_partition(NULL) returns the slot that's NOT
    // currently the boot partition. On a fresh factory image (ota_data
    // unwritten) ESP-IDF picks ota_0; after first boot from ota_0 this
    // returns ota_1.
    inactivePartition_ = esp_ota_get_next_update_partition(NULL);
    if (inactivePartition_ == nullptr)
    {
        ESP_LOGE(
            TAG,
            "handleBegin: esp_ota_get_next_update_partition returned NULL — partition table is missing ota_0/ota_1");
        logSendResult("handleBegin NO_PARTITION NAK", sendBeginNak(mac, xferId, OtaBeginNakReason::NO_PARTITION));
        return;
    }

    if (msg.begin.totalSize > inactivePartition_->size)
    {
        ESP_LOGW(TAG, "handleBegin: totalSize=%u exceeds partition '%s' size=%u — NAK NO_PARTITION",
                 (unsigned)msg.begin.totalSize, inactivePartition_->label, (unsigned)inactivePartition_->size);
        inactivePartition_ = nullptr;
        logSendResult("handleBegin NO_PARTITION (oversize) NAK",
                      sendBeginNak(mac, xferId, OtaBeginNakReason::NO_PARTITION));
        return;
    }

    // OTA_SIZE_UNKNOWN tells esp_ota_begin to erase sectors lazily inside
    // esp_ota_write. Passing the exact totalSize would erase the entire
    // reserved span up front — a ~2 MB (8MB board) / ~6.4 MB (16MB board)
    // one-shot erase that can exceed the BEGIN_ACK timeout window.
    esp_err_t bErr = esp_ota_begin(inactivePartition_, OTA_SIZE_UNKNOWN, &otaHandle_);
    if (bErr != ESP_OK)
    {
        ESP_LOGE(TAG, "handleBegin: esp_ota_begin failed: %s — NAK BEGIN_FAILED", esp_err_to_name(bErr));
        otaHandle_ = 0;
        inactivePartition_ = nullptr;
        logSendResult("handleBegin BEGIN_FAILED (esp_ota_begin) NAK",
                      sendBeginNak(mac, xferId, OtaBeginNakReason::BEGIN_FAILED));
        return;
    }

    // Must equal the master's BulkSender kWindowSize. Mismatched windows
    // desync ack accounting.
    constexpr uint8_t kWindowSize = 8;
    auto br = bulk_.begin(xferId, msg.begin.totalSize, msg.begin.totalChunks, msg.begin.chunkSize, kWindowSize);
    if (!br.valid)
    {
        ESP_LOGW(TAG, "handleBegin: BulkReceiver::begin rejected: reason=%d (totalSize=%u chunks=%u chunkSize=%u)",
                 (int)br.reason, (unsigned)msg.begin.totalSize, (unsigned)msg.begin.totalChunks,
                 (unsigned)msg.begin.chunkSize);
        // esp_ota_begin succeeded but BulkReceiver setup failed —
        // resetOtaHandleAndSha clears the partial state.
        resetOtaHandleAndSha();
        logSendResult("handleBegin BEGIN_FAILED (BulkReceiver) NAK",
                      sendBeginNak(mac, xferId, OtaBeginNakReason::BEGIN_FAILED));
        return;
    }

    AstrOsSha256_init(&shaCtx_);
    shaActive_ = true;

    currentXferId_ = xferId;
    memcpy(currentMasterMac_, mac, sizeof(currentMasterMac_));
    currentTotalSize_ = msg.begin.totalSize;
    currentTotalChunks_ = msg.begin.totalChunks;
    memcpy(expectedSha256_, msg.begin.sha256Expected, sizeof(expectedSha256_));

    // Reset stats counters before the first wire activity.
    statsLastRecvSeq_ = 0;
    statsHighestAckedSeq_ = 0;
    statsAnyAcked_ = false;
    statsNaksCRC_ = 0;
    statsNaksSIZE_ = 0;
    statsNaksOOO_ = 0;
    statsNaksFLASH_ = 0;
    statsSendFailCount_ = 0;

    active_ = true;

    ESP_LOGI(
        TAG,
        "handleBegin accepted: xferId=%u totalSize=%u chunks=%u chunkSize=%u partition='%s' (size=%u, offset=0x%lx)",
        xferId, (unsigned)msg.begin.totalSize, (unsigned)msg.begin.totalChunks, (unsigned)msg.begin.chunkSize,
        inactivePartition_->label, (unsigned)inactivePartition_->size, (unsigned long)inactivePartition_->address);

    // If the ACK frame never even got enqueued, the master will hit its
    // BEGIN_ACK timeout and abandon (OtaForwarder::handleBeginNak). Leaving
    // active_=true would force an operator retry to wait out the 10 s
    // watchdog before getting anything but a BUSY NAK. Release state now.
    esp_err_t ackErr = sendBeginAck(mac, xferId);
    logSendResult("handleBegin BEGIN_ACK", ackErr);
    if (ackErr != ESP_OK)
    {
        ESP_LOGW(TAG, "handleBegin: sendBeginAck failed — releasing writer state so a retry can begin");
        statsSendFailCount_++;
        resetOtaHandleAndSha();
        return;
    }
    watchdogStart();
    statsTimerStart();
}

void OtaWriter::handleData(queue_ota_writer_msg_t &msg)
{
    const uint8_t *mac = msg.data.srcMac;
    const uint8_t xferId = msg.data.xferId;
    const uint32_t seq = msg.data.seq;

    if (!active_)
    {
        // Inactive NAK: chunk arrived while no transfer is live. Use the
        // wire's xferId (not currentXferId_) since the master is still
        // tagging by its in-flight transfer. Zero hint fields signal
        // "receiver inactive" — non-zero would invite the master to
        // rewind to a seq we don't track.
        ESP_LOGW(TAG, "handleData: chunk for xferId=%u seq=%u arrived while inactive — NAK OUT_OF_ORDER", xferId, seq);
        logSendResult("handleData inactive NAK",
                      sendDataNak(mac, xferId, /*hcs=*/0, /*nes=*/0, /*wr=*/0, OtaDataNakReason::OUT_OF_ORDER));
        return;
    }

    // Track highest seq seen on the wire (monotonic — late/duplicate
    // packets don't regress the progress indicator).
    if (seq > statsLastRecvSeq_)
        statsLastRecvSeq_ = seq;

    auto cr = bulk_.onChunk(xferId, seq, msg.data.payloadLen, msg.data.crc16, msg.data.payload);

    if (cr.decision == AstrOsBulkTransport::Decision::NAK)
    {
        OtaDataNakReason wireReason = OtaDataNakReason::OUT_OF_ORDER;
        switch (cr.reason)
        {
        case AstrOsBulkTransport::NakReason::CRC:
            wireReason = OtaDataNakReason::CRC;
            statsNaksCRC_++;
            break;
        case AstrOsBulkTransport::NakReason::SIZE:
            wireReason = OtaDataNakReason::SIZE;
            statsNaksSIZE_++;
            break;
        case AstrOsBulkTransport::NakReason::OUT_OF_ORDER:
            wireReason = OtaDataNakReason::OUT_OF_ORDER;
            statsNaksOOO_++;
            break;
        case AstrOsBulkTransport::NakReason::FLASH_FULL:
            // BulkReceiver's receive-side capacity exhausted — terminal on
            // our end. Map to WRITE so the master abandons rather than
            // retransmitting into a writer that has no room.
            ESP_LOGW(TAG, "handleData: NAK reason=FLASH_FULL — wire-encoding as WRITE (terminal)");
            wireReason = OtaDataNakReason::WRITE;
            statsNaksFLASH_++;
            break;
        case AstrOsBulkTransport::NakReason::NONE:
            // Shouldn't happen on a NAK decision; fall through to OUT_OF_ORDER
            // as the safest hint for the master.
            ESP_LOGW(TAG, "handleData: NAK with reason=NONE — wire-encoding as OUT_OF_ORDER");
            wireReason = OtaDataNakReason::OUT_OF_ORDER;
            statsNaksOOO_++;
            break;
        }
        ESP_LOGW(TAG, "handleData: xferId=%u seq=%u NAK reason=%d (hcs=%u nes=%u wr=%u)", xferId, seq, (int)wireReason,
                 (unsigned)cr.highestContiguousSeq, (unsigned)cr.nextExpectedSeq, (unsigned)cr.windowRemaining);
        esp_err_t nakErr =
            sendDataNak(mac, xferId, cr.highestContiguousSeq, cr.nextExpectedSeq, cr.windowRemaining, wireReason);
        logSendResult("handleData chunk NAK", nakErr);
        if (nakErr != ESP_OK)
            statsSendFailCount_++;
        // Silence-based watchdog: a transient NAK (CRC/SIZE/OUT_OF_ORDER/NONE)
        // still proves master is alive and retransmitting, so reset the idle
        // timer. FLASH_FULL is the exception — it's wire-encoded as terminal
        // WRITE so master abandons; leaving the watchdog ticking ensures
        // cleanup if master ignores the WRITE NAK and keeps sending.
        if (cr.reason != AstrOsBulkTransport::NakReason::FLASH_FULL)
        {
            watchdogRestart();
        }
        return;
    }

    esp_err_t wErr = esp_ota_write(otaHandle_, cr.payload, cr.payloadLen);
    if (wErr != ESP_OK)
    {
        ESP_LOGE(TAG, "handleData: esp_ota_write failed: %s — aborting transfer xferId=%u seq=%u",
                 esp_err_to_name(wErr), xferId, seq);
        // Terminal failure: send WRITE NAK with zero hint fields. The wire
        // contract treats windowRemaining=0 as "receiver inactive" — that
        // matches our post-reset state and prevents the master from
        // retransmitting into a torn-down writer.
        esp_err_t nakErr = sendDataNak(mac, xferId, /*hcs=*/0, /*nes=*/0, /*wr=*/0, OtaDataNakReason::WRITE);
        logSendResult("handleData WRITE NAK (esp_ota_write)", nakErr);
        if (nakErr != ESP_OK)
            statsSendFailCount_++;
        resetOtaHandleAndSha();
        return;
    }

    if (shaActive_)
    {
        AstrOsSha256_update(&shaCtx_, cr.payload, cr.payloadLen);
    }
    else
    {
        // Should be unreachable: handleBegin sets shaActive_ on the same
        // path that opens otaHandle_. Reaching here means the END compare
        // will trip HASH_MISMATCH.
        ESP_LOGE(TAG, "handleData: shaActive_=false on accepted chunk — END will report HASH_MISMATCH");
    }

    ESP_LOGD(TAG, "handleData: xferId=%u seq=%u accepted (cum=%u next=%u wr=%u)", xferId, seq,
             (unsigned)cr.highestContiguousSeq, (unsigned)cr.nextExpectedSeq, (unsigned)cr.windowRemaining);

    // Stats: chunk accepted, so the wire's cumulative ACK is authoritative.
    statsHighestAckedSeq_ = cr.highestContiguousSeq;
    statsAnyAcked_ = true;

    esp_err_t ackErr = sendDataAck(mac, xferId, cr.highestContiguousSeq, cr.nextExpectedSeq, cr.windowRemaining);
    logSendResult("handleData DATA_ACK", ackErr);
    if (ackErr != ESP_OK)
        statsSendFailCount_++;
    watchdogRestart();
}

void OtaWriter::handleEnd(queue_ota_writer_msg_t &msg)
{
    const uint8_t *mac = msg.end.srcMac;
    const uint8_t xferId = msg.end.xferId;

    if (!active_)
    {
        // Silent drop: WRITE_ERROR is reserved for actual flash-write
        // failures; replying it here would corrupt the master's forensic
        // signal. Master's END_ACK timeout will recover the transfer
        // status. Same discipline as handleWatchdogFire — don't reply
        // into a likely-dead mesh.
        ESP_LOGI(TAG, "handleEnd: xferId=%u arrived while inactive — silent drop (master END_ACK timeout will recover)",
                 xferId);
        return;
    }

    // BulkReceiver::onEnd validates totalChunksSent matches the BEGIN's
    // totalChunks. Mismatch indicates a protocol-level desync that we
    // can't recover from (the master sent fewer chunks than it claimed).
    auto er = bulk_.onEnd(xferId, msg.end.totalChunksSent);
    bool teardownRequested = AstrOsBulkTransport::shouldTeardownOnEndResult(er);
    if (er.status != AstrOsBulkTransport::EndResult::Status::OK)
    {
        ESP_LOGW(TAG, "handleEnd: BulkReceiver::onEnd rejected: reason=%d", (int)er.reason);
        uint8_t zero[32] = {0};
        logSendResult("handleEnd onEnd-rejected END_ACK", sendEndAck(mac, xferId, OtaEndStatus::WRITE_ERROR, zero));
        if (teardownRequested)
        {
            resetOtaHandleAndSha();
        }
        return;
    }

    // 1. Finalize streaming SHA.
    uint8_t streamedDigest[32];
    if (shaActive_)
    {
        AstrOsSha256_final(&shaCtx_, streamedDigest);
        shaActive_ = false;
    }
    else
    {
        ESP_LOGE(TAG, "handleEnd: shaActive_=false at finalize — emitting all-zero digest as HASH_MISMATCH");
        memset(streamedDigest, 0, sizeof(streamedDigest));
    }

    if (memcmp(streamedDigest, expectedSha256_, sizeof(streamedDigest)) != 0)
    {
        ESP_LOGE(TAG, "handleEnd: streaming SHA mismatch — replying HASH_MISMATCH (chunks=%u, totalSize=%u)",
                 (unsigned)msg.end.totalChunksSent, (unsigned)currentTotalSize_);
        logSendResult("handleEnd HASH_MISMATCH END_ACK",
                      sendEndAck(mac, xferId, OtaEndStatus::HASH_MISMATCH, streamedDigest));
        resetOtaHandleAndSha();
        return;
    }

    // esp_ota_end validates the image header (magic byte + size). With
    // secure boot disabled (our config) that's the only check. Does NOT
    // activate the new partition — a follow-up step owns the boot-table
    // flip.
    esp_err_t eErr = esp_ota_end(otaHandle_);
    if (eErr != ESP_OK)
    {
        ESP_LOGE(TAG, "handleEnd: esp_ota_end failed: %s — replying WRITE_ERROR", esp_err_to_name(eErr));
        // esp_ota_end has already released the handle internally even on
        // failure (per IDF docs); zero the handle so resetOtaHandleAndSha
        // doesn't try to re-abort it.
        otaHandle_ = 0;
        logSendResult("handleEnd WRITE_ERROR (esp_ota_end) END_ACK",
                      sendEndAck(mac, xferId, OtaEndStatus::WRITE_ERROR, streamedDigest));
        resetOtaHandleAndSha();
        return;
    }
    otaHandle_ = 0;

    // Read-back-and-rehash: catches silent flash corruption that landed
    // between esp_ota_write and esp_ota_end's finalize. 4 KB buffer
    // matches the flash sector size. Stack-allocated; sized against
    // otaWriterTask's stack (8 KB). Re-verify HWM if the stack is shrunk.
    AstrOsSha256Ctx rbCtx;
    AstrOsSha256_init(&rbCtx);

    constexpr size_t kReadBufSize = 4096;
    uint8_t buf[kReadBufSize];
    bool readbackOk = true;
    for (size_t off = 0; off < currentTotalSize_; off += kReadBufSize)
    {
        size_t chunk = (currentTotalSize_ - off < kReadBufSize) ? (currentTotalSize_ - off) : kReadBufSize;
        esp_err_t rErr = esp_partition_read(inactivePartition_, off, buf, chunk);
        if (rErr != ESP_OK)
        {
            ESP_LOGE(TAG, "handleEnd: esp_partition_read at off=%zu len=%zu failed: %s", off, chunk,
                     esp_err_to_name(rErr));
            readbackOk = false;
            break;
        }
        AstrOsSha256_update(&rbCtx, buf, chunk);
    }

    uint8_t readbackDigest[32];
    AstrOsSha256_final(&rbCtx, readbackDigest);

    if (!readbackOk)
    {
        logSendResult("handleEnd WRITE_ERROR (readback IO) END_ACK",
                      sendEndAck(mac, xferId, OtaEndStatus::WRITE_ERROR, streamedDigest));
        resetOtaHandleAndSha();
        return;
    }

    if (memcmp(readbackDigest, streamedDigest, sizeof(readbackDigest)) != 0)
    {
        ESP_LOGE(TAG,
                 "handleEnd: READ-BACK hash mismatch — flash write silently corrupted bytes; replying WRITE_ERROR");
        // Send the readback digest (not streamed) so master forensics can
        // see what's actually on flash vs what we expected.
        logSendResult("handleEnd WRITE_ERROR (readback mismatch) END_ACK",
                      sendEndAck(mac, xferId, OtaEndStatus::WRITE_ERROR, readbackDigest));
        resetOtaHandleAndSha();
        return;
    }

    ESP_LOGI(TAG, "handleEnd: transfer xferId=%u OK — %u bytes verified on partition '%s'", xferId,
             (unsigned)currentTotalSize_, inactivePartition_->label);

    // Stop watchdog and stats timer before the 2 s delay so neither
    // fires while we're sleeping. resetOtaHandleAndSha() at the end of
    // this function handles the rest of the per-transfer teardown.
    watchdogStop();
    statsTimerStop();

    logSendResult("handleEnd OK END_ACK", sendEndAck(mac, xferId, OtaEndStatus::OK, streamedDigest));

    // M4 PLACEHOLDER — see .docs/plans/20260527-0143-firmware-ota-progress-emission-design.md
    //
    // Verification passed. Send OTA_END_ACK OK immediately (above) so the
    // master can emit FW_PROGRESS FLASHING and the UI flash row lights up.
    // Then deliberately delay 2 s so that flash row is visibly current
    // before reporting the M4 placeholder failure.
    //
    // PR set 2 replaces this block with:
    //   - vTaskDelay(pdMS_TO_TICKS(2000));
    //   - esp_err_t err = esp_ota_set_boot_partition(inactivePartition_);
    //   - sendFlashResult(mac, xferId,
    //                     err == ESP_OK ? OtaFlashStatus::OK : OtaFlashStatus::FAILED,
    //                     err == ESP_OK ? "" : esp_err_to_name(err));
    //   - if (err == ESP_OK) esp_restart();
    //
    // The vTaskDelay below mirrors the natural PR set 2 cadence so server
    // timing assumptions tested against M4 hold unchanged against PR set 2.
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_err_t flashErr = sendFlashResult(mac, xferId, OtaFlashStatus::FLASH_NOT_IMPLEMENTED, "pr_set_1_placeholder");
    if (flashErr != ESP_OK)
    {
        ESP_LOGW(TAG, "handleEnd: sendFlashResult failed: %s", esp_err_to_name(flashErr));
    }

    resetOtaHandleAndSha();
}

void OtaWriter::handleWatchdogFire()
{
    if (!active_)
    {
        // Late fire: handleEnd or a NAK-side reset already cleaned up,
        // and the watchdog signal landed in the queue before watchdogStop
        // took effect. No-op — but log so a misfiring timer is visible.
        ESP_LOGI(TAG, "handleWatchdogFire: no active transfer; treating as late signal");
        return;
    }
    ESP_LOGE(TAG, "handleWatchdogFire: idle threshold (%llums) exceeded for xferId=%u — aborting transfer",
             kWatchdogIdleUs / 1000ULL, currentXferId_);
    // No reply to master — the watchdog fires when the master is unreachable
    // or has already abandoned the transfer (its own BEGIN_ACK / data-ack /
    // END_ACK timeouts will have triggered). Sending a NAK into a dead
    // mesh wastes bandwidth and risks confusing a fresh transfer that
    // happens to be using the same xferId from a different master incarnation.
    resetOtaHandleAndSha();
}

esp_err_t OtaWriter::sendBeginAck(const uint8_t mac[6], uint8_t xferId)
{
    OtaBeginAckPayload p{};
    p.xferId = xferId;
    return AstrOs_EspNow.sendOtaFrame(mac, AstrOsPacketType::OTA_BEGIN_ACK, reinterpret_cast<const uint8_t *>(&p),
                                      sizeof(p));
}

esp_err_t OtaWriter::sendBeginNak(const uint8_t mac[6], uint8_t xferId, OtaBeginNakReason reason)
{
    OtaBeginNakPayload p{};
    p.xferId = xferId;
    p.reason = static_cast<uint8_t>(reason);
    return AstrOs_EspNow.sendOtaFrame(mac, AstrOsPacketType::OTA_BEGIN_NAK, reinterpret_cast<const uint8_t *>(&p),
                                      sizeof(p));
}

esp_err_t OtaWriter::sendDataAck(const uint8_t mac[6], uint8_t xferId, uint32_t highestContiguousSeq,
                                 uint32_t nextExpectedSeq, uint8_t windowRemaining)
{
    OtaDataAckPayload p{};
    p.xferId = xferId;
    p.highestContiguousSeq = highestContiguousSeq;
    p.nextExpectedSeq = nextExpectedSeq;
    p.windowRemaining = windowRemaining;
    return AstrOs_EspNow.sendOtaFrame(mac, AstrOsPacketType::OTA_DATA_ACK, reinterpret_cast<const uint8_t *>(&p),
                                      sizeof(p));
}

esp_err_t OtaWriter::sendDataNak(const uint8_t mac[6], uint8_t xferId, uint32_t highestContiguousSeq,
                                 uint32_t nextExpectedSeq, uint8_t windowRemaining, OtaDataNakReason reason)
{
    OtaDataNakPayload p{};
    p.xferId = xferId;
    p.highestContiguousSeq = highestContiguousSeq;
    p.nextExpectedSeq = nextExpectedSeq;
    p.windowRemaining = windowRemaining;
    p.reason = static_cast<uint8_t>(reason);
    return AstrOs_EspNow.sendOtaFrame(mac, AstrOsPacketType::OTA_DATA_NAK, reinterpret_cast<const uint8_t *>(&p),
                                      sizeof(p));
}

esp_err_t OtaWriter::sendEndAck(const uint8_t mac[6], uint8_t xferId, OtaEndStatus status,
                                const uint8_t sha256Computed[32])
{
    OtaEndAckPayload p{};
    p.xferId = xferId;
    p.status = static_cast<uint8_t>(status);
    memcpy(p.sha256Computed, sha256Computed, sizeof(p.sha256Computed));
    return AstrOs_EspNow.sendOtaFrame(mac, AstrOsPacketType::OTA_END_ACK, reinterpret_cast<const uint8_t *>(&p),
                                      sizeof(p));
}

esp_err_t OtaWriter::sendFlashResult(const uint8_t mac[6], uint8_t xferId, OtaFlashStatus status,
                                     const std::string &reason)
{
    OtaFlashResultPayload payload{};
    payload.xferId = xferId;
    payload.status = static_cast<uint8_t>(status);
    payload.reasonLen = static_cast<uint8_t>(std::min<size_t>(reason.size(), sizeof(payload.reason)));
    if (payload.reasonLen > 0)
    {
        std::memcpy(payload.reason, reason.data(), payload.reasonLen);
    }
    return AstrOs_EspNow.sendOtaFrame(mac, AstrOsPacketType::OTA_FLASH_RESULT,
                                      reinterpret_cast<const uint8_t *>(&payload), sizeof(payload));
}
