// ============================================================
// LoRa APRS digipeater for the STM32WLE5 (Ebyte E77-400MBL-01)
//
// Digipeat only: listens all the time and repeats standard LoRa APRS frames (3C FF 01 + TNC2 text) with
// the path rewritten (fill-in or wide, the rule the LoRa APRS Pi uses). Optional "moving stations only"
// mode, a fixed-position beacon, settings typed on the serial port or sent over the air. No GPS, no iGate.
//
// Radio set-up as in PicoTrack and LoRa-Tracker-Digipeater (K6ATV): RF switch on PA6/PA7/PB3, TCXO 1.7 V.
// Settings: typed over the serial port (help, show, set, save), kept in the last flash page; src/digi3/config_digi3.h holds the defaults.
// Core logic (tested on a PC): src/digi3/digi3_core.c, settings.c.
// ============================================================

#include <Arduino.h>
#include <RadioLib.h>
#include <IWatchdog.h>
#include "digi3_core.h"
#include "neighbors.h"
#include "config_digi3.h"
#include "settings.h"
#include "remote.h"
#include "battery.h"

// LOG: events (every packet heard, every transmission). Compiled out when DEBUG_SERIAL is 0, and silenced at run time by "set log off".
// OUT: always printed (command replies, boot messages, the counters on request).
#if DEBUG_SERIAL
  #define LOG(...)   do { if (cfg.log) Serial.print(__VA_ARGS__); } while (0)
  #define LOGLN(...) do { if (cfg.log) Serial.println(__VA_ARGS__); } while (0)
#else
  #define LOG(...)
  #define LOGLN(...)
#endif
#define OUT(...)   Serial.print(__VA_ARGS__)
#define OUTLN(...) Serial.println(__VA_ARGS__)

STM32WLx radio = new STM32WLx_Module();
static const uint32_t               rfswitch_pins[]  = RFSWITCH_PINS;
static const Module::RfSwitchMode_t rfswitch_table[] = { RFSWITCH_TABLE };

static d3_settings_t cfg;            // the settings in use (defaults, then the saved record, then whatever was typed)
static d3_settings_t radioApplied;   // the radio settings the chip is really using
static d3_settings_t savedCfg;       // what is in flash (a command over the air saves its counter on top of this, not the unsaved changes)
static bool          dirty = false;  // typed but not saved
static char myCall[16];
static bool callsignSet = false;

// ---- received-packet flag (set in the radio interrupt) ----
static volatile bool     rxFlag = false;
static volatile uint32_t rxEndMs = 0;
static void onRadioRx() { rxFlag = true; rxEndMs = millis(); }

// ---- transmit queue ----
struct TxSlot {
    bool     used;
    uint8_t  freeChecks; // channel found free this many times in a row
    uint8_t  usedHops;   // hops used in the path when we heard it (cancel-on-hear)
    uint32_t waitStartMs;// first channel check (0 = not yet)
    uint32_t dueMs;      // earliest time to transmit or check the channel again
    uint32_t rxEndMs;    // end of the reception this repeats; 0 for our own packets (cancel-on-hear only looks at repeats)
    uint8_t  own;        // 0 a repeat, 1 our beacon, 2 a message (reply or ack to a command over the air)
    d3_job_t job;
};
static TxSlot txq[DIGI_QUEUE_SIZE];

// ---- duplicates ----
struct Seen { uint32_t key, ms; };
static Seen seen[DEDUPE_SLOTS];
static uint8_t seenNext = 0;

// ---- counters ----
static uint32_t nBusy = 0, nCancelled = 0, nSkipStationary = 0, nSkipOther = 0;
static uint32_t nHeard = 0, nCrc = 0, nRelayPlain = 0, nDup = 0, nBusyDrop = 0, nQueueFull = 0, nBeacons = 0;

// ---- stations heard ----
static d3_station_t   stationSlots[NEIGHBOR_SLOTS];
static d3_neighbors_t neighbors;
static d3_moving_cfg_t movingCfg;
static uint32_t cadSlotMs;           // one channel check: about 4 symbols
static uint32_t nWhy[D3_REASONS];

static uint32_t nextBeaconMs = 0, nextStatsMs = 0;

// ---- RX LED: flashes when a packet is heard (LED2 shows transmissions by itself, see config_digi3.h) ----
static bool     ledOn = false;
static uint32_t ledOffMs = 0;
static void ledRx(uint32_t ms)
{
    if (!cfg.leds) return;
    digitalWrite(LED_RX_PIN, HIGH); ledOn = true; ledOffMs = millis() + ms;
}
static void ledPoll()
{
    if (ledOn && (int32_t)(millis() - ledOffMs) >= 0) { digitalWrite(LED_RX_PIN, LOW); ledOn = false; }
}
// ---- settings over the air ----
static uint8_t  rcKey[16];
static uint8_t  rcFails = 0;
static uint32_t rcLockUntil = 0, rebootAtMs = 0, nRemoteOk = 0, nRemoteBad = 0;
static char     rcLastFrom[10], rcLastReply[68];
static uint32_t rcLastMs = 0;

// ---- battery (battery.h) ----
static d3b_t    battery;
static unsigned battMv = 0;                       // the last reading that was believable (0 = none yet)
static uint32_t nextBattMs = 0, simMv = 0, simLeft = 0;   // simLeft: readings still replaced by the simulated voltage (battery sim)
static void battText(char *out, size_t n, unsigned mv);

// ============================================================
// Everything derived from the settings; run after any change.
static void applySettings()
{
    d3s_fullcall(&cfg, myCall, sizeof myCall);
    callsignSet = strcmp(cfg.call, "N0CALL") != 0;
    movingCfg.move_threshold_m = cfg.move_m; movingCfg.stationary_repeat_min = cfg.stationary_min;
    movingCfg.repeat_messages = cfg.mv_messages; movingCfg.repeat_other = cfg.mv_other; 
    movingCfg.always_tocalls = cfg.always_tocalls;
    cadSlotMs = (4000UL * (1UL << cfg.sf) + cfg.bw_hz / 2) / cfg.bw_hz;
    if (cfg.secret[0]) d3r_key_from_secret(cfg.secret, rcKey);
    else d3r_key_from_uid(HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2(), rcKey);
}

static bool handleRemote(const char *text, uint32_t now);

static float mhz(uint32_t hz) { return (float)(hz / 1000000u) + (float)(hz % 1000000u) / 1e6f; }

static bool seenRecently(uint32_t key, uint32_t now)
{
    for (int i = 0; i < DEDUPE_SLOTS; i++)
        if (seen[i].ms && seen[i].key == key && now - seen[i].ms < cfg.dedupe_s * 1000UL) return true;
    return false;
}

static void remember(uint32_t key, uint32_t now)
{
    seen[seenNext].key = key;
    seen[seenNext].ms  = now ? now : 1;
    seenNext = (seenNext + 1) % DEDUPE_SLOTS;
}

static TxSlot *freeSlot()
{
    for (int i = 0; i < DIGI_QUEUE_SIZE; i++) if (!txq[i].used) return &txq[i];
    return nullptr;
}

static uint32_t randomBetween(uint32_t lo, uint32_t hi) { return lo + (uint32_t)random((long)(hi - lo + 1)); }

// ============================================================
// RADIO
// ============================================================
// The radio settings that can change while running (frequency, bandwidth, SF, CR, power, sync word, preamble, gain).
static int16_t radioSetParams(const d3_settings_t &c)
{
    int16_t st = radio.setFrequency(mhz(c.freq_hz));
    if (st == RADIOLIB_ERR_NONE) st = radio.setBandwidth(c.bw_hz / 1000.0f);
    if (st == RADIOLIB_ERR_NONE) st = radio.setSpreadingFactor(c.sf);
    if (st == RADIOLIB_ERR_NONE) st = radio.setCodingRate(c.cr);
    if (st == RADIOLIB_ERR_NONE) st = radio.setOutputPower(c.power_dbm);
    if (st == RADIOLIB_ERR_NONE) st = radio.setSyncWord(c.sync);
    if (st == RADIOLIB_ERR_NONE) st = radio.setPreambleLength(c.preamble);
    if (st == RADIOLIB_ERR_NONE) st = radio.setRxBoostedGainMode(c.boosted ? true : false);
    return st;
}

static bool radioDiffers()
{
    return cfg.freq_hz != radioApplied.freq_hz || cfg.bw_hz != radioApplied.bw_hz || cfg.sf != radioApplied.sf || cfg.cr != radioApplied.cr ||
           cfg.power_dbm != radioApplied.power_dbm || cfg.sync != radioApplied.sync || cfg.preamble != radioApplied.preamble ||
           cfg.boosted != radioApplied.boosted;
}

// Start the radio from cfg. Returns the RadioLib status; does not reset (the caller decides what to do about a failure).
static int16_t radioInit()
{
    radio.setRfSwitchTable(rfswitch_pins, rfswitch_table);
    LOG(F("[Radio] init... "));
    int16_t st = radio.begin(mhz(cfg.freq_hz), cfg.bw_hz / 1000.0f, cfg.sf, cfg.cr);
    if (st == RADIOLIB_ERR_NONE) st = radio.setTCXO(TCXO_VOLTAGE);
    if (st == RADIOLIB_ERR_NONE) st = radio.setCurrentLimit(LORA_OCP_MA);
    if (st == RADIOLIB_ERR_NONE) st = radioSetParams(cfg);
    if (st == RADIOLIB_ERR_NONE) st = radio.setCRC(true);
    if (st == RADIOLIB_ERR_NONE) {
        radio.setPacketReceivedAction(onRadioRx);
        radioApplied = cfg;
        LOGLN(F("OK"));
    } else {
        OUT(F("[Radio] init failed, code ")); OUTLN(st);
    }
    return st;
}

static void startRx()
{
    rxFlag = false;
    int16_t st = radio.startReceive();
    if (st != RADIOLIB_ERR_NONE) { LOG(F("[Radio] startReceive failed ")); LOGLN(st); }
}

// ============================================================
// RECEIVE
// ============================================================
static void handleRx()
{
    uint8_t  fr[256];
    uint32_t endMs = rxEndMs;
    size_t   n = radio.getPacketLength();
    if (n > sizeof fr) n = sizeof fr;
    int16_t st = radio.readData(fr, n);
    float rssi = radio.getRSSI(), snr = radio.getSNR(), fo = radio.getFrequencyError();
    startRx();                                  // listen again at once; the work below needs no radio
    if (st == RADIOLIB_ERR_CRC_MISMATCH) { nCrc++; ledRx(40); return; }
    if (st != RADIOLIB_ERR_NONE || n < 4) return;
    nHeard++;
    ledRx(150);
    uint32_t now = millis();

    if (cfg.cancel_on_hear) {   // another digipeater repeated a packet we are still waiting to repeat: drop ours
        uint32_t key; int usedNow;
        if (d3_identify(fr, n, &key, &usedNow))
            for (int i = 0; i < DIGI_QUEUE_SIZE; i++)
                if (txq[i].used && txq[i].rxEndMs && txq[i].job.key == key && usedNow > txq[i].usedHops) {
                    txq[i].used = false; nCancelled++;
                    LOG(F("[Digi] cancelled, already repeated by another digi: ")); LOGLN(txq[i].job.text);
                }
    }

    TxSlot *slot = freeSlot();
    static d3_job_t scratch;                    // decided here first, so a full queue still counts the reason
    d3_job_t *job = slot ? &slot->job : &scratch;
    int r = callsignSet ? d3_decide(fr, n, myCall, cfg.mode, job) : D3_NOT_SERVED;
    if (r == D3_OK && battery.state >= D3B_LOW) r = D3_BATTERY;      // low battery: read and counted, not repeated
    const char *text = d3_last_text();          // the packet as received, if this digi could read it
    if (text[0]) d3_observe(&neighbors, text, strlen(text), rssi, snr, now);
    if (text[0] && handleRemote(text, now)) return;   // a message to this digi is answered, not repeated
    LOG(F("[RX] ")); LOG(n); LOG(F(" B ")); LOG(rssi, 0); LOG(F(" dBm ")); LOG(snr, 1); LOG(F(" dB ")); LOG((long)fo); LOG(F(" Hz: "));
#if D3_LOG_RX_HEADER
    if (text[0]) {                              // the packet as received: source>destination,path (a digipeater's callsign in the path shows who repeated it; * = used)
        char hdr[96]; const char *colon = strchr(text, ':'); size_t hl = colon ? (size_t)(colon - text) : strlen(text);
        if (hl > sizeof hdr - 1) hl = sizeof hdr - 1;
        memcpy(hdr, text, hl); hdr[hl] = 0;
        LOG(hdr); LOG(F(" - "));
    }
#endif
    if (r != D3_OK) { nWhy[r]++; LOGLN(d3_reason(r)); return; }
    if (seenRecently(job->key, now)) { nDup++; LOGLN(F("duplicate")); return; }
    remember(job->key, now);
    if (cfg.moving) {
        int md = d3_moving_decision(&neighbors, &movingCfg, text[0] ? text : nullptr, strlen(text), job->named, now);
        if (!d3_move_repeats(md)) {
            if (md == D3_MOVE_SKIP_STATIONARY) nSkipStationary++; else nSkipOther++;
            LOGLN(d3_move_reason(md));
            return;
        }
        LOG(d3_move_reason(md)); LOG(F(": "));
    }
    if (!slot) { nQueueFull++; LOGLN(F("queue full")); return; }
    slot->used = true; slot->freeChecks = 0; slot->waitStartMs = 0; slot->usedHops = job->used_hops;
    slot->dueMs = now + randomBetween(cfg.delay_min_ms, cfg.delay_max_ms);
    slot->rxEndMs = endMs ? endMs : now; slot->own = 0;
    LOG(F("repeat ")); LOGLN(job->text);
}

// ============================================================
// TRANSMIT
// ============================================================
static void transmitDue()
{
    uint32_t now = millis();
    for (int i = 0; i < DIGI_QUEUE_SIZE; i++) {
        TxSlot &s = txq[i];
        if (!s.used || (int32_t)(now - s.dueMs) < 0) continue;
        if (rxFlag) return;                     // a packet waiting to be read: the channel is busy, and it may cancel this repeat

        // Listen before talk: CAD_DIFS_SLOTS free checks in a row; a busy check backs off 1 to CAD_BACKOFF_MAX slots.
        if (!s.waitStartMs) s.waitStartMs = now ? now : 1;
        if (now - s.waitStartMs > cfg.cad_wait_s * 1000UL) {
            s.used = false; nBusyDrop++;
            LOGLN(F("[TX] channel busy too long, given up"));
            if (s.own == 1) nextBeaconMs = now + 30000UL;          // our beacon: try again in 30 s
            return;
        }
        int16_t cad = radio.scanChannel();
        startRx();                              // keep receiving between checks
        if (cad != RADIOLIB_CHANNEL_FREE) {
            nBusy++; s.freeChecks = 0;
            s.dueMs = millis() + cadSlotMs * randomBetween(1, cfg.cad_backoff);
            return;
        }
        if (++s.freeChecks < cfg.cad_difs) { s.dueMs = millis(); return; }

        uint8_t out[D3_MAX_FRAME]; size_t len;
        len = d3_build(&s.job, out, sizeof out);
        s.used = false;
        if (!len) { startRx(); return; }

        IWatchdog.reload();
        int16_t st = radio.transmit(out, len);
        IWatchdog.reload();
        if (st == RADIOLIB_ERR_NONE) {
            if (s.own == 1) nBeacons++;
            else if (s.own) {}
            else nRelayPlain++;
            LOG(F("[TX] ")); LOG(len); LOGLN(F(" B sent"));
        } else {
            LOG(F("[TX] failed, code ")); LOGLN(st);
        }
        startRx();
        return;                                 // one transmission per pass, then listen again
    }
}

// ============================================================
// BEACON: fixed position, plain text
// ============================================================
static void latlon(char *out, size_t cap, int32_t lat_u, int32_t lon_u)
{
    uint32_t a = ((uint32_t)(lat_u < 0 ? -lat_u : lat_u) * 6u + 500u) / 1000u;       // hundredths of a minute
    uint32_t o = ((uint32_t)(lon_u < 0 ? -lon_u : lon_u) * 6u + 500u) / 1000u;
    snprintf(out, cap, "%02lu%02lu.%02lu%c%c%03lu%02lu.%02lu%c%c",
             (unsigned long)(a / 6000), (unsigned long)((a % 6000) / 100), (unsigned long)(a % 100), lat_u >= 0 ? 'N' : 'S', cfg.sym_table[0],
             (unsigned long)(o / 6000), (unsigned long)((o % 6000) / 100), (unsigned long)(o % 100), lon_u >= 0 ? 'E' : 'W', cfg.sym_code[0]);
}

static void queueBeacon()
{
    if (battery.state >= D3B_LOW) { LOGLN(F("[Beacon] skipped: battery low")); return; }
    TxSlot *slot = freeSlot();
    if (!slot) return;
    char pos[32];
    latlon(pos, sizeof pos, cfg.lat_u, cfg.lon_u);
    int w = snprintf(slot->job.text, sizeof slot->job.text, "%s>%s%s%s:!%s%s", myCall, cfg.tocall,
                     cfg.path[0] ? "," : "", cfg.path, pos, cfg.comment);
    if (w <= 0 || w >= (int)sizeof slot->job.text) return;
    slot->job.text_len = (uint16_t)w;
    slot->used = true; slot->freeChecks = 0; slot->waitStartMs = 0; slot->usedHops = 0; slot->dueMs = millis(); slot->rxEndMs = 0; slot->own = 1;
    LOG(F("[Beacon] ")); LOGLN(slot->job.text);
}

// An APRS message from this digi: ":TO       :text" (plain text, so any station can read it).
static bool queueMessage(const char *to, const char *text, uint32_t delayMs)
{
    TxSlot *slot = freeSlot();
    if (!slot) return false;
    int w = snprintf(slot->job.text, sizeof slot->job.text, "%s>%s%s%s::%-9s:%.67s", myCall, cfg.tocall, cfg.rc_path[0] ? "," : "", cfg.rc_path, to, text);
    if (w <= 0 || w >= (int)sizeof slot->job.text) return false;
    slot->job.text_len = (uint16_t)w;
    slot->used = true; slot->freeChecks = 0; slot->waitStartMs = 0; slot->usedHops = 0; slot->dueMs = millis() + delayMs; slot->rxEndMs = 0; slot->own = 2;
    LOG(F("[Remote] reply ")); LOGLN(slot->job.text);
    return true;
}

static void printStats()
{
    OUT(F("[Stats] heard ")); OUT(nHeard); OUT(F(", CRC errors ")); OUT(nCrc);
    OUT(F(", repeated ")); OUT(nRelayPlain);
    OUT(F(", duplicates ")); OUT(nDup); OUT(F(", not LoRa APRS text ")); OUT(nWhy[D3_NOT_APRS]);
    OUT(F(", damaged ")); OUT(nWhy[D3_BAD_FRAME]); OUT(F(", busy drops ")); OUT(nBusyDrop);
    OUT(F(", queue full ")); OUT(nQueueFull); OUT(F(", beacons ")); OUT(nBeacons);
    OUT(F(", channel busy ")); OUT(nBusy); OUT(F(", cancelled ")); OUT(nCancelled);
    OUT(F(", fixed stations skipped ")); OUT(nSkipStationary); OUT(F(", other skipped ")); OUTLN(nSkipOther);
    // The station list: heard on the air by this digi (signal), and where each station last reported itself.
    uint32_t now = millis();
    for (int i = 0; i < neighbors.n; i++) {
        const d3_station_t &st = neighbors.s[i];
        if (!st.call[0]) continue;
        OUT(F("  ")); OUT(st.call);
        if (st.rf_ms) { OUT(F("  heard ")); OUT((now - st.rf_ms) / 60000UL); OUT(F(" min ago ")); OUT(st.rssi10 / 10); OUT(F(" dBm ")); OUT(st.snr10 / 10.0f, 1); OUT(F(" dB")); OUT(st.direct_src ? F(" direct") : F("")); }
        if (st.has_pos) { OUT(F("  at ")); OUT(st.lat / 1e6, 5); OUT(F(",")); OUT(st.lon / 1e6, 5); OUT(F(" ")); OUT((now - st.pos_ms) / 60000UL); OUT(F(" min ago")); }
        if (st.has_rep) { OUT(F("  last repeated ")); OUT((now - st.rep_ms) / 60000UL); OUT(F(" min ago")); }
        OUTLN();
    }
    OUT(F("[Stats] commands over the air: ")); OUT(nRemoteOk); OUT(F(" accepted, ")); OUT(nRemoteBad); OUTLN(F(" with a wrong code"));
    if (cfg.batt) { char bt[48]; battText(bt, sizeof bt, battMv); OUT(F("[Stats] battery ")); OUTLN(bt); }
}

// ============================================================
// SETTINGS IN FLASH: the last 2 KB page of the chip. platformio.ini lowers the linker's flash size by one page so the program
// can never grow into it. One record, with a CRC; an erased or damaged page just means "use the compiled-in defaults".
// ============================================================
#define FLASH_PAGE_BYTES 2048u

static uint32_t settingsAddr()
{
    uint32_t kb = *(const uint16_t *)FLASHSIZE_BASE;
    return kb == 256 ? FLASH_BASE + kb * 1024u - FLASH_PAGE_BYTES : 0;   // the linker reservation in platformio.ini assumes 256 KB
}

static void loadSettings()
{
    d3s_defaults(&cfg);
    uint32_t addr = settingsAddr();
    if (!addr) { OUTLN(F("[Settings] unexpected flash size: saving is off, using the compiled-in defaults")); return; }
    int rej = 0;
    int r = d3s_unpack((const uint8_t *)addr, D3S_REC_MAX, &cfg, &rej);
    if (r == D3S_E_BLANK)       OUTLN(F("[Settings] none saved yet, using the compiled-in defaults"));
    else if (r < 0)             OUTLN(F("[Settings] the saved record is damaged: using the compiled-in defaults"));
    else { OUT(F("[Settings] loaded ")); OUT(r); OUT(F(" from flash")); if (rej) { OUT(F(", ")); OUT(rej); OUT(F(" refused (out of range)")); } OUTLN(); }
    savedCfg = cfg;
}

// Erase the page and write one record; read it back. Returns true when the flash now holds exactly rec.
static bool writeRecord(const uint8_t *rec, size_t n)
{
    uint32_t addr = settingsAddr();
    if (!addr) return false;
    if (memcmp((const void *)addr, rec, n) == 0) return true;
    radio.standby();                            // no radio activity while the flash is erased and written
    IWatchdog.reload();
    HAL_FLASH_Unlock();
    FLASH_EraseInitTypeDef er = {};
    er.TypeErase = FLASH_TYPEERASE_PAGES;
    er.Page      = (addr - FLASH_BASE) / FLASH_PAGE_BYTES;
    er.NbPages   = 1;
    uint32_t pageErr = 0;
    bool ok = HAL_FLASHEx_Erase(&er, &pageErr) == HAL_OK;
    for (size_t i = 0; ok && i < n; i += 8) {
        uint64_t dw; memcpy(&dw, rec + i, 8);
        ok = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr + i, dw) == HAL_OK;
    }
    HAL_FLASH_Lock();
    IWatchdog.reload();
    startRx();
    return ok && memcmp((const void *)addr, rec, n) == 0;
}

// Keep the counter of the last command over the air across a restart, without saving any setting that was typed but not saved.
static void saveCounter(uint32_t counter)
{
    uint8_t rec[D3S_REC_MAX];
    savedCfg.rc_counter = counter;
    size_t n = d3s_pack(&savedCfg, rec, sizeof rec);
    if (!n || !writeRecord(rec, n)) OUTLN(F("[Remote] error: could not save the counter"));
}

static void saveSettings()
{
    uint32_t addr = settingsAddr();
    if (!addr) { OUTLN(F("error: this chip's flash size is not the 256 KB the settings page assumes")); return; }
    if (radioDiffers()) { OUTLN(F("error: radio settings changed but not applied: type apply first, so a setting the radio refuses is never saved")); return; }
    uint8_t rec[D3S_REC_MAX];
    size_t n = d3s_pack(&cfg, rec, sizeof rec);
    if (!n) { OUTLN(F("error: the record does not fit")); return; }
    if (memcmp((const void *)addr, rec, n) == 0) { dirty = false; savedCfg = cfg; OUTLN(F("already saved, nothing written")); return; }
    bool ok = writeRecord(rec, n);
    if (ok) { dirty = false; savedCfg = cfg; OUT(F("saved ")); OUT((unsigned long)n); OUTLN(F(" bytes to flash")); }
    else    { OUT(F("error: flash write failed, code ")); OUTLN((unsigned long)HAL_FLASH_GetError()); }
}

// ============================================================
// BATTERY: the chip's own supply measured against its internal reference (the battery is wired straight to the 3.3 V pin).
// What the voltage means is in battery.c; this part reads it, and puts the radio and the processor to sleep when the battery is empty.
// ============================================================
extern "C" void SystemClock_Config(void);
static RTC_HandleTypeDef hrtc;
static volatile bool rtcWoke = false;
extern "C" void RTC_WKUP_IRQHandler(void) { HAL_RTCEx_WakeUpTimerIRQHandler(&hrtc); }
extern "C" void HAL_RTCEx_WakeUpTimerEventCallback(RTC_HandleTypeDef *) { rtcWoke = true; }

static unsigned readVddMv()
{
    if (simLeft) { simLeft--; return (unsigned)simMv; }
    analogReadResolution(12);
    uint32_t sum = 0;
    for (int i = 0; i < 8; i++) sum += analogRead(AVREF);
    unsigned mv = d3b_vdd_mv(*VREFINT_CAL_ADDR, (sum + 4) / 8);
    return (mv >= 1500 && mv <= 4500) ? mv : 0;           // 0: not a believable reading, ignored
}

static d3b_limits_t battLimits()
{
    d3b_limits_t l = { cfg.batt_warn_mv, cfg.batt_low_mv, cfg.batt_sleep_mv, cfg.batt_resume_mv };
    return l;
}

// "3.31 V, ok"
static void battText(char *out, size_t n, unsigned mv)
{
    if (!mv) { snprintf(out, n, "no reading yet"); return; }
    snprintf(out, n, "%u.%02u V, %s%s", mv / 1000, (mv % 1000) / 10, cfg.batt ? d3b_name(battery.state) : "not watched", mv > 3600 ? ", above 3.6 V: check the charger" : "");
}

static void rtcWakeInit()                                // the RTC wake-up timer, from the LSI (it also runs the watchdog)
{
    static bool done = false;
    if (done) return;
    done = true;
    RCC_OscInitTypeDef o = {};
    o.OscillatorType = RCC_OSCILLATORTYPE_LSI; o.LSIState = RCC_LSI_ON;
    HAL_RCC_OscConfig(&o);
    RCC_PeriphCLKInitTypeDef p = {};
    p.PeriphClockSelection = RCC_PERIPHCLK_RTC; p.RTCClockSelection = RCC_RTCCLKSOURCE_LSI;
    HAL_RCCEx_PeriphCLKConfig(&p);
    __HAL_RCC_RTC_ENABLE();
    hrtc.Instance = RTC;
    hrtc.Init.HourFormat = RTC_HOURFORMAT_24; hrtc.Init.AsynchPrediv = 127; hrtc.Init.SynchPrediv = 255;
    hrtc.Init.OutPut = RTC_OUTPUT_DISABLE; hrtc.Init.OutPutRemap = RTC_OUTPUT_REMAP_NONE;
    hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH; hrtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;
    HAL_RTC_Init(&hrtc);
    HAL_NVIC_SetPriority(RTC_WKUP_IRQn, 3, 0);
    HAL_NVIC_EnableIRQ(RTC_WKUP_IRQn);
}

// Empty battery: the radio sleeps, the processor stops (STOP2, a few microamps) and wakes every 8 s to read the supply. Returns when the
// battery is back above the resume voltage. The watchdog keeps counting while stopped, so it is fed at every wake-up (8 s against 20 s).
static void batterySleep()
{
    OUTLN(F("[Battery] empty: the radio and the processor sleep; the supply is checked every 8 s (serial port off until it recovers)"));
    Serial.flush();
    radio.sleep(false);                                  // cold start: the lowest current; the radio is set up again when waking
    digitalWrite(LED_RX_PIN, LOW); ledOn = false;
    memset(txq, 0, sizeof txq);
    rcLockUntil = 0;
    rtcWakeInit();
    d3b_limits_t lim = battLimits();
    while (battery.state == D3B_SLEEP) {
        IWatchdog.reload();
        rtcWoke = false;
        HAL_RTCEx_SetWakeUpTimer_IT(&hrtc, 16000, RTC_WAKEUPCLOCK_RTCCLK_DIV16, 0);     // 16000 / (about 32 kHz / 16) = about 8 s
        HAL_SuspendTick();
        HAL_PWREx_EnterSTOP2Mode(PWR_STOPENTRY_WFI);
        SystemClock_Config();                            // after STOP the processor is on its slow start-up clock
        HAL_ResumeTick();
        HAL_RTCEx_DeactivateWakeUpTimer(&hrtc);
        IWatchdog.reload();
        if (!rtcWoke) continue;                          // woken by something else: sleep again without a reading
        unsigned mv = readVddMv();
        if (mv) { battMv = mv; d3b_update(&battery, mv, &lim); }
    }
    char t[48]; battText(t, sizeof t, battMv);
    OUT(F("[Battery] recovered: ")); OUTLN(t);
    if (radioInit() != RADIOLIB_ERR_NONE) { OUTLN(F("[Radio] did not start after the battery sleep: restarting")); delay(200); NVIC_SystemReset(); }
    rxFlag = false;
    startRx();
    nextBeaconMs = millis() + 30000UL;
}

// One reading through the states; acts on a change.
static void batteryApply(unsigned mv)
{
    battMv = mv;
    d3b_limits_t lim = battLimits();
    d3b_state_t was = battery.state;
    if (!d3b_update(&battery, mv, &lim)) return;
    char t[48]; battText(t, sizeof t, mv);
    OUT(F("[Battery] ")); OUTLN(t);
    if (battery.state >= D3B_LOW && was < D3B_LOW)       // no more repeats or beacons (answers to commands over the air still go out)
        for (int i = 0; i < DIGI_QUEUE_SIZE; i++) if (txq[i].used && txq[i].own != 2) txq[i].used = false;
    if (battery.state == D3B_OK && was >= D3B_LOW) nextBeaconMs = millis() + 30000UL;
    if (battery.state == D3B_SLEEP) batterySleep();
}

static void batteryPoll(uint32_t now)
{
    if (!cfg.batt) { if (battery.state != D3B_OK) { d3b_init(&battery); OUTLN(F("[Battery] watching is off: back to normal")); } return; }
    if ((int32_t)(now - nextBattMs) < 0) return;
    nextBattMs = now + 10000UL;
    unsigned mv = readVddMv();
    if (mv) batteryApply(mv);
}

// "battery" and "battery sim MV SECONDS"
static void handleBattery(const char *args)
{
    while (*args == ' ') args++;
    if (strncmp(args, "sim", 3) == 0) {
        if (strncmp(args + 3, " off", 4) == 0) { simLeft = 0; OUTLN(F("ok: simulation off")); return; }
        char *e1, *e2;
        unsigned long mv = strtoul(args + 3, &e1, 10), s = strtoul(e1, &e2, 10);
        if (e1 == args + 3 || e2 == e1 || mv < 1500 || mv > 4500 || s == 0 || s > 3600) { OUTLN(F("error: battery sim MILLIVOLTS SECONDS (for testing: the next readings are replaced by that voltage; battery sim off ends it)")); return; }
        simMv = mv; simLeft = (s + 9) / 10;
        OUT(F("ok: the readings are replaced by ")); OUT(mv); OUT(F(" mV for about ")); OUT(s); OUTLN(F(" s"));
        return;
    }
    if (*args) { OUTLN(F("error: battery, or battery sim MILLIVOLTS SECONDS")); return; }
    char t[64]; battText(t, sizeof t, readVddMv());
    OUT(F("battery ")); OUTLN(t);
    OUT(F("  warn ")); OUT(cfg.batt_warn_mv); OUT(F(" mV, low ")); OUT(cfg.batt_low_mv); OUT(F(" mV, sleep ")); OUT(cfg.batt_sleep_mv);
    OUT(F(" mV, back to normal at ")); OUT(cfg.batt_resume_mv); OUT(F(" mV; watching is ")); OUTLN(cfg.batt ? F("on") : F("off"));
}

// ============================================================
// SERIAL COMMANDS
// ============================================================
static void emitLine(void *, const char *l) { OUTLN(l); }

static void applyRadio()
{
    radio.standby();
    int16_t st = radioSetParams(cfg);
    if (st == RADIOLIB_ERR_NONE) { radioApplied = cfg; OUTLN(F("ok: radio settings applied")); }
    else {
        OUT(F("error: the radio refused those settings (code ")); OUT(st); OUTLN(F("); going back to the ones in use"));
        cfg.freq_hz = radioApplied.freq_hz; cfg.bw_hz = radioApplied.bw_hz; cfg.sf = radioApplied.sf; cfg.cr = radioApplied.cr;
        cfg.power_dbm = radioApplied.power_dbm; cfg.sync = radioApplied.sync; cfg.preamble = radioApplied.preamble; cfg.boosted = radioApplied.boosted;
        radioSetParams(cfg);
    }
    applySettings();
    startRx();
}

static void handleCommand(const char *line)
{
    if (strncasecmp(line, "battery", 7) == 0 && (line[7] == 0 || line[7] == ' ')) { handleBattery(line + 7); return; }
#if SERIAL_SEND
    if (strncasecmp(line, "send ", 5) == 0) {             // send CALL text: an APRS message from this digi (the setup page's remote control)
        char to[10]; const char *p = line + 5; size_t n = 0;
        while (*p && *p != ' ' && n < sizeof to - 1) to[n++] = (char)toupper((unsigned char)*p++);
        to[n] = 0; while (*p == ' ') p++;
        if (!callsignSet) OUTLN(F("error: set a callsign first"));
        else if (!n || !*p) OUTLN(F("error: send CALL text"));
        else OUTLN(queueMessage(to, p, 0) ? F("ok: message queued") : F("error: no room, try again"));
        return;
    }
#endif
    char before[16]; strcpy(before, myCall);
    unsigned fl = d3s_exec(&cfg, line, emitLine, nullptr);
    if (fl & D3S_CHANGED) {
        applySettings();
        if (!cfg.leds) { digitalWrite(LED_RX_PIN, LOW); ledOn = false; }
        if (strcmp(before, myCall) != 0) memset(txq, 0, sizeof txq);      // packets queued under the old callsign are dropped
        if (!dirty) OUTLN(F("(not saved yet: type save to keep it after a power-off)"));
        dirty = true;
        if (!callsignSet) OUTLN(F("(callsign is still N0CALL: nothing is relayed or beaconed)"));
    }
    if (fl & D3S_APPLY)  applyRadio();
    if (fl & D3S_SAVE)   saveSettings();
    if (fl & D3S_STATS)  printStats();
    if (fl & D3S_BEACON) {
        if (!callsignSet) OUTLN(F("error: set a callsign first"));
        else if (cfg.lat_u == 0 && cfg.lon_u == 0) OUTLN(F("error: no position set (set lat, set lon)"));
        else { queueBeacon(); OUTLN(F("ok: beacon queued")); }
    }
    if (fl & D3S_REBOOT) {
        if (dirty && !(fl & D3S_FORCE)) OUTLN(F("there are unsaved changes: type save first, or reboot force to drop them"));
        else { OUTLN(F("restarting...")); Serial.flush(); delay(50); NVIC_SystemReset(); }
    }
}

// ============================================================
// SETTINGS OVER THE AIR: an APRS message to this callsign, "<counter> <command> <code>" (remote.h)
// ============================================================
static char rcReply[68];
static void emitRemote(void *, const char *l)
{
    OUT(F("[Remote] ")); OUTLN(l);
    if (!rcReply[0]) { strncpy(rcReply, strncmp(l, "ok: ", 4) == 0 ? l + 4 : l, sizeof rcReply - 1); rcReply[sizeof rcReply - 1] = 0; }
}

static void appendReply(const char *t)
{
    size_t n = strlen(rcReply);
    if (n < sizeof rcReply - 1) { strncpy(rcReply + n, t, sizeof rcReply - 1 - n); rcReply[sizeof rcReply - 1] = 0; }
}

static void sendReply(const char *from, const char *msgno, const char *reply)
{
    char ack[12];
    if (msgno[0]) { snprintf(ack, sizeof ack, "ack%s", msgno); queueMessage(from, ack, 1500); }
    queueMessage(from, reply, 2000);
}

// True when the packet was a message to this digi (then it is not repeated).
static bool handleRemote(const char *text, uint32_t now)
{
    const char *gt = strchr(text, '>'), *colon = strchr(text, ':');
    if (!gt || !colon || gt > colon || gt - text > 9) return false;
    char from[10], body[100], msgno[8], command[80], code[12];
    memcpy(from, text, gt - text); from[gt - text] = 0;
    if (!d3r_message_to(colon + 1, strlen(colon + 1), myCall, body, sizeof body, msgno, sizeof msgno)) return false;
    if (!cfg.remote || !callsignSet || strcasecmp(from, myCall) == 0) return true;
    if (strncmp(body, "ack", 3) == 0 || strncmp(body, "rej", 3) == 0) return true;      // an ack to one of our replies
    uint32_t counter;
    if (!d3r_split(body, &counter, command, sizeof command, code, sizeof code)) { LOG(F("[Remote] message, not a command: ")); LOGLN(body); return true; }
    if (rcLockUntil && (int32_t)(now - rcLockUntil) < 0) { LOGLN(F("[Remote] locked out after wrong codes, ignored")); return true; }
    rcLockUntil = 0;
    if (!d3r_check(rcKey, myCall, counter, command, code)) {
        nRemoteBad++;
        OUT(F("[Remote] wrong code from ")); OUTLN(from);
        if (++rcFails >= REMOTE_MAX_FAILS) { rcFails = 0; rcLockUntil = (now + REMOTE_LOCKOUT_MIN * 60000UL) | 1; OUTLN(F("[Remote] too many wrong codes: commands ignored for a while")); }
        return true;
    }
    rcFails = 0;
    if (counter == cfg.rc_counter && strcasecmp(from, rcLastFrom) == 0) {      // the same command again: answer again, do not run it again
        if (now - rcLastMs > 20000UL) { sendReply(from, msgno, rcLastReply); rcLastMs = now; }   // (a second copy through another digi within 20 s: no)
        return true;
    }
    if (counter <= cfg.rc_counter || counter > 0x7FFFFFFFUL) { OUT(F("[Remote] old counter (a replay?) from ")); OUTLN(from); return true; }

    nRemoteOk++;
    OUT(F("[Remote] ")); OUT(from); OUT(F(": ")); OUTLN(command);
    cfg.rc_counter = counter;                   // in a save made by this command too
    rcReply[0] = 0;
    const char *why = d3s_remote_check(command);
    unsigned fl = 0;
    if (strcasecmp(command, "get battery") == 0) { char bt[40]; battText(bt, sizeof bt, readVddMv()); snprintf(rcReply, sizeof rcReply, "battery %s", bt); }
    else if (why) snprintf(rcReply, sizeof rcReply, "error: %s", why);
    else fl = d3s_exec(&cfg, command, emitRemote, nullptr);
    if (fl & D3S_CHANGED) { applySettings(); dirty = true; appendReply(" (not saved)"); }
    if (fl & D3S_SAVE) { saveSettings(); snprintf(rcReply, sizeof rcReply, dirty ? "error: not saved" : "saved"); }
    if (fl & D3S_BEACON) { if (battery.state >= D3B_LOW) snprintf(rcReply, sizeof rcReply, "error: battery low"); else if (cfg.lat_u || cfg.lon_u) { queueBeacon(); snprintf(rcReply, sizeof rcReply, "beacon sent"); } else snprintf(rcReply, sizeof rcReply, "error: no position set"); }
    if (fl & D3S_REBOOT) {
        if (dirty && !(fl & D3S_FORCE)) snprintf(rcReply, sizeof rcReply, "unsaved changes: save first, or reboot force");
        else { snprintf(rcReply, sizeof rcReply, "restarting"); rebootAtMs = (now + 20000UL) | 1; }     // after the reply has gone out
    }
    if (!rcReply[0]) snprintf(rcReply, sizeof rcReply, "ok");
    if (savedCfg.rc_counter != counter) saveCounter(counter);
    strcpy(rcLastFrom, from); strcpy(rcLastReply, rcReply); rcLastMs = now;
    sendReply(from, msgno, rcReply);
    return true;
}

static char    lineBuf[D3S_LINE_MAX + 1];
static uint8_t lineLen = 0;
static bool    lineOver = false;

static void serialPoll()
{
    while (Serial.available() > 0) {
        int c = Serial.read();
        if (c == '\r' || c == '\n') {
            if (lineLen || lineOver) {
                OUTLN();
                lineBuf[lineLen] = 0;
                if (lineOver) OUTLN(F("error: line too long")); else handleCommand(lineBuf);
                lineLen = 0; lineOver = false;
            }
        } else if (c == 8 || c == 127) {
            if (lineLen) { lineLen--; OUT(F("\b \b")); }
        } else if (c >= 32 && c < 127) {
            if (lineLen < D3S_LINE_MAX) { lineBuf[lineLen++] = (char)c; Serial.write((uint8_t)c); } else lineOver = true;
        }
    }
}

#if KEYS_ENABLED
// The chip's own bootloader lives in system memory. Called first thing in setup(), before the watchdog and the serial
// port are started, so the bootloader finds the chip as after a reset.
static void jumpToBootloader()
{
    const uint32_t BOOT = 0x1FFF0000UL;
    __disable_irq();
    HAL_RCC_DeInit(); HAL_DeInit();
    SysTick->CTRL = 0; SysTick->LOAD = 0; SysTick->VAL = 0;
    for (int i = 0; i < 2; i++) { NVIC->ICER[i] = 0xFFFFFFFFUL; NVIC->ICPR[i] = 0xFFFFFFFFUL; }
    __HAL_SYSCFG_REMAPMEMORY_SYSTEMFLASH();
    SCB->VTOR = 0;
    __set_MSP(*(volatile uint32_t *)BOOT);
    ((void (*)(void))(*(volatile uint32_t *)(BOOT + 4)))();
    for (;;) {}
}
static bool keyHeld(uint32_t pin) { return digitalRead(pin) == LOW; }
#endif

// ============================================================
void setup()
{
#if KEYS_ENABLED
    pinMode(KEY_DEFAULTS_PIN, INPUT_PULLUP); pinMode(KEY_BOOTLOADER_PIN, INPUT_PULLUP);
    delay(10);                                  // the keys have a debounce capacitor
    if (keyHeld(KEY_BOOTLOADER_PIN)) jumpToBootloader();
    const bool restoreDefaults = keyHeld(KEY_DEFAULTS_PIN);
#endif
#if D3_DEBUG_SWD
    HAL_DBGMCU_EnableDBGSleepMode();            // first, before anything can sleep: keeps SWD answering in WFI
    HAL_DBGMCU_EnableDBGStopMode();             // batterySleep() uses STOP2, which wakes only every 8 s
#endif
    Serial.begin(DEBUG_BAUD);                   // always on: the settings commands use it
    delay(300);
    OUTLN(F("=== LoRa APRS digipeater (STM32WLE5) ==="));
    if (IWatchdog.isReset(true)) OUTLN(F("[Boot] restarted by the watchdog"));
    IWatchdog.begin((uint32_t)WATCHDOG_S * 1000000UL);

    pinMode(LED_RX_PIN, OUTPUT);                // out of its power-on debug mode, which has a pull-up that glows the LED faintly
    digitalWrite(LED_RX_PIN, LOW);
    loadSettings();
#if KEYS_ENABLED
    if (restoreDefaults) { uint32_t ctr = cfg.rc_counter; d3s_defaults(&cfg); cfg.rc_counter = ctr; OUTLN(F("[Boot] key held: the compiled-in defaults are back (saved below)")); }
#endif
    applySettings();
    OUT(F("[Boot] callsign ")); OUTLN(myCall);
    {
        char id[28]; snprintf(id, sizeof id, "%08lX%08lX%08lX", (unsigned long)HAL_GetUIDw0(), (unsigned long)HAL_GetUIDw1(), (unsigned long)HAL_GetUIDw2());
        OUT(F("[Boot] settings over the air ")); OUT(cfg.remote ? F("on") : F("off"));
        OUT(F(", key: ")); if (cfg.secret[0]) OUTLN(F("the secret you set")); else { OUT(F("chip ID ")); OUTLN(id); }
    }
    if (!callsignSet) OUTLN(F("[Boot] callsign is N0CALL: nothing is relayed or beaconed. Type  set call YOURCALL-SSID  then  save  (help lists everything)"));
    OUTLN();
    d3s_menu(emitLine, nullptr);
    OUTLN();

    memset(txq, 0, sizeof txq);
    d3_neighbors_init(&neighbors, stationSlots, NEIGHBOR_SLOTS);
    memset(seen, 0, sizeof seen);
    if (radioInit() != RADIOLIB_ERR_NONE) {
        // a saved radio setting the chip refuses must not leave a remote site dead: go back to the compiled-in radio values
        d3_settings_t def; d3s_defaults(&def);
        cfg.freq_hz = def.freq_hz; cfg.bw_hz = def.bw_hz; cfg.sf = def.sf; cfg.cr = def.cr; cfg.power_dbm = def.power_dbm;
        cfg.sync = def.sync; cfg.preamble = def.preamble; cfg.boosted = def.boosted;
        applySettings();
        OUTLN(F("[Radio] trying the compiled-in radio settings"));
        if (radioInit() != RADIOLIB_ERR_NONE) { delay(2000); NVIC_SystemReset(); }   // try again from the start instead of hanging
        OUTLN(F("[Radio] running on the compiled-in radio settings; the saved ones were refused"));
    }
    randomSeed(radio.random(0x7FFFFFFF));
    startRx();
#if KEYS_ENABLED
    if (restoreDefaults) saveSettings();
#endif
    d3b_init(&battery);
    if (cfg.batt) {                                      // three quick readings, so an empty battery is noticed at power-up too
        for (int i = 0; i < BATT_CONFIRM; i++) { unsigned mv = readVddMv(); if (mv) batteryApply(mv); delay(20); }
        char t[48]; battText(t, sizeof t, battMv);
        OUT(F("[Boot] battery ")); OUTLN(t);
    }
    nextBattMs = millis() + 10000UL;

    nextBeaconMs = millis() + cfg.beacon_first_s * 1000UL;
    nextStatsMs  = millis() + (cfg.stats_min ? cfg.stats_min : 10) * 60000UL;
}

void loop()
{
    IWatchdog.reload();
    serialPoll();
    ledPoll();
    if (rxFlag) handleRx();
    transmitDue();

    uint32_t now = millis();
    batteryPoll(now);
    if (callsignSet && battery.state < D3B_LOW && (cfg.lat_u != 0 || cfg.lon_u != 0) && (int32_t)(now - nextBeaconMs) >= 0) {
        queueBeacon();
        nextBeaconMs = now + cfg.beacon_min * 60000UL;
    }
    if (rebootAtMs && (int32_t)(now - rebootAtMs) >= 0) {                  // asked over the air: the reply had its 20 s to go out
        bool pending = false; for (int i = 0; i < DIGI_QUEUE_SIZE; i++) if (txq[i].used && txq[i].own == 2) pending = true;
        if (!pending || (int32_t)(now - rebootAtMs) > 30000) { OUTLN(F("restarting...")); Serial.flush(); delay(50); NVIC_SystemReset(); }
    }
    if ((int32_t)(now - nextStatsMs) >= 0) {
        if (cfg.stats_min && cfg.log) printStats();
        nextStatsMs = now + (cfg.stats_min ? cfg.stats_min : 10) * 60000UL;
    }

    // Sleep until the next interrupt (the radio, the serial port, or the 1 ms system tick). The radio keeps listening.
    HAL_PWR_EnterSLEEPMode(PWR_MAINREGULATOR_ON, PWR_SLEEPENTRY_WFI);
}
