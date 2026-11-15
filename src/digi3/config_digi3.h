#pragma once
// ============================================================
// LoRa APRS digipeater (digipeat only: no GPS, no iGate, no tracking)
// Target: Ebyte E77-400MBL-01 (STM32WLE5CC)
// Build:  PlatformIO environment "e77_digi"
//
// These values are the DEFAULTS. Over the serial port (115200 baud) "help" lists commands that change nearly all of them
// ("set call K6ATV-2", "set mode wide", ..., then "save" to keep them in flash); a saved record overrides the defaults here.
// Fixed at compile time: TCXO, RF switch, current limit, queue and table sizes, watchdog, serial speed.
// ============================================================

// ---- IDENTITY ------------------------------------------------
// The digipeater neither relays nor beacons until a real callsign is set (here, or over the serial port: set call K6ATV-2).
#define MY_CALLSIGN         "N0CALL"
#define MY_SSID             10              // 0 = no SSID

// ---- DIGIPEATER ----------------------------------------------
//   D3_MODE_OFF     repeats nothing (it still beacons and answers settings messages)
//   D3_MODE_FILLIN  WIDE1-1 and packets addressed to this callsign (a low, local fill-in digi)
//   D3_MODE_WIDE    also WIDE2-1 and WIDE2-2 (a high site that covers a wide area)
#define DIGI_MODE           D3_MODE_WIDE
#define DIGI_DELAY_MIN_MS   100             // random wait before repeating, so two digis rarely key at once
#define DIGI_DELAY_MAX_MS   500
#define DIGI_QUEUE_SIZE     4

// ---- LISTEN BEFORE TALK (as the LoRa APRS Pi and the iGate firmware) ----
// Before every transmission the channel must be found free CAD_DIFS_SLOTS times in a row (one slot = one channel
// activity check, about 4 symbols: 131 ms at SF12). A busy channel means a random wait of 1 to CAD_BACKOFF_MAX slots,
// still receiving. After CAD_MAX_WAIT_S without a free channel a repeat is given up (a beacon is tried again in 30 s).
#define CAD_DIFS_SLOTS      2
#define CAD_BACKOFF_MAX     4
#define CAD_MAX_WAIT_S      10

// ---- CANCEL ON HEAR ----
// A repeat still waiting (for its delay or a free channel) is dropped when the same packet is heard repeated by
// another digipeater first (more used hops in its path than when this digi heard it).
#define CANCEL_ON_HEAR      1

// ---- MOVING STATIONS ONLY ----
// 1: repeat only what moves, plus whatever asks for this digi by name. Every packet heard updates a list of
// stations (position, when last heard, signal), printed with the counters on the serial port.
//   Always repeated: a path that names this digi (e.g. a tracker set to path K6ATV-10), and destinations (tocalls)
//   that start with a prefix in ALWAYS_REPEAT_TOCALLS.
//   Repeated: a station's first position, and any position MOVE_THRESHOLD_M or more from the last one repeated.
//   Not repeated: a fixed station heard again in the same place (unless STATIONARY_REPEAT_MIN is set).
// 0: an ordinary digipeater (the path rule alone decides). The station list is kept either way.
#define DIGI_ONLY_MOVING          0
#define MOVE_THRESHOLD_M          100
#define STATIONARY_REPEAT_MIN     0         // also repeat a fixed station's position after this many minutes (0 = never)
#define MOVING_REPEAT_MESSAGES    1         // messages and acks (they have no position)
#define MOVING_REPEAT_OTHER       0         // status, telemetry and other packets without a position
#define ALWAYS_REPEAT_TOCALLS     ""        // e.g. "APLAIX,APLAB" (prefixes, comma-separated)
#define NEIGHBOR_SLOTS            48        // stations remembered (about 60 bytes each)
#define DEDUPE_SECONDS      30              // the same packet heard again within this time is not repeated again
#define DEDUPE_SLOTS        32

// Show the received packet's source>destination,path on each [RX] log line (about 90 bytes of flash).
// A build that is short of flash can define it as 0 (-D D3_LOG_RX_HEADER=0).
#ifndef D3_LOG_RX_HEADER
#define D3_LOG_RX_HEADER    1
#endif

// ---- BEACON (fixed position, plain text so every receiver can read it) ----
// Leave LAT/LON at 0 to send no beacon.
#define BEACON_LAT          0.0             // degrees, north positive
#define BEACON_LON          0.0             // degrees, east positive
#define BEACON_SYMBOL_TABLE 'L'             // 'L' overlay on the digipeater symbol: a LoRa digi
#define BEACON_SYMBOL_CODE  '#'
#define BEACON_COMMENT      "LoRa APRS digi, STM32WLE5"
#define BEACON_PATH         ""              // e.g. "WIDE1-1"; a high digi usually beacons with no path
#define BEACON_INTERVAL_MIN 60
#define BEACON_FIRST_S      30              // the first beacon after power-up
// Destination (tocall): experimental APZ until APLAB? is assigned for the STM32WLE5 projects.
#define APRS_DESTINATION    "APZLD3"

// ---- SETTINGS OVER THE AIR ------------------------------------
// An APRS message to this callsign can run get, set, save, beacon or reboot (not the callsign, the radio settings or the secret).
// Each message carries a code made with tools/remote_code.py from the secret: by default this chip's unique ID, printed at start-up;
// "set secret ..." on the serial port replaces it. The command stays readable, the secret never goes on the air.
#define REMOTE_COMMANDS     1
#define REMOTE_REPLY_PATH   "WIDE1-1"       // path of the replies: repeated once, so a sender out of direct range still hears them ("" = direct)
#define REMOTE_MAX_FAILS    5               // wrong codes in a row before commands are ignored for a while
#define REMOTE_LOCKOUT_MIN  10

// ---- LORA (LoRa APRS standard: 433.775 MHz, 125 kHz, SF12, CR 4/5, sync word 0x12) ----
#define LORA_FREQ           433.775
#define LORA_BW             125.0
#define LORA_SF             12
#define LORA_CR             5
#define LORA_TX_POWER       22              // dBm, the STM32WL's maximum
#define LORA_OCP_MA         140
#define LORA_SYNC_WORD      0x12
#define LORA_PREAMBLE       8
// Boosted receive gain: about 2 to 3 dB more sensitivity for about 0.7 mA. At a noisy site it gains nothing.
#define RX_BOOSTED_GAIN     1

// ---- TCXO -----------------------------------------------------
// E77 modules with SN >= 3202995 have a TCXO. Same value as PicoTrack and LoRa-Tracker-Digipeater.
#define TCXO_VOLTAGE        1.7f

// ---- RF SWITCH: E77 MBL board (verified on PicoTrack) ------------
#define RFSWITCH_PINS   {PA6, PA7, PB3, RADIOLIB_NC, RADIOLIB_NC}
#define RFSWITCH_TABLE  \
    {STM32WLx::MODE_IDLE,  {LOW,  LOW,  LOW}},  \
    {STM32WLx::MODE_RX,    {LOW,  HIGH, LOW}},  \
    {STM32WLx::MODE_TX_LP, {HIGH, LOW,  HIGH}}, \
    {STM32WLx::MODE_TX_HP, {HIGH, LOW,  HIGH}}, \
    END_OF_MODE_TABLE

// ---- LEDs (Ebyte MBL baseboard) ----
// LED1 (PB4) flashes for every packet heard: long for a good packet, short for a CRC error.
// LED2 (PB3) is wired to the RF switch line that is high only while transmitting, so it lights for every transmission
// by itself; it cannot be switched off by "set leds off". Fit the jumpers from PB4 to LED1 and PB3 to LED2.
#define LEDS_ENABLED        1               // default of the "leds" setting
#define LED_RX_PIN          PB4

// ---- KEYS (Ebyte MBL baseboard: Key 1 / Key 2 on PA0 / PA1, active low; which label is which is not confirmed) ----
// Read once at power-up or reset. Held on PA0: put the compiled-in defaults back and save them (the rescue for bad
// radio settings or a forgotten secret). Held on PA1: jump to the chip's own serial bootloader (USART2 on PA2/PA3), so
// the firmware can be updated over the USB cable. If both are held, the bootloader wins. 0 removes the code.
#define KEYS_ENABLED        1
#define KEY_DEFAULTS_PIN    PA0
#define KEY_BOOTLOADER_PIN  PA1

// ---- BATTERY (the chip's own supply, so the battery must be wired to the board's 3.3 V pin; a LiFePO4 cell is about 3.2 V flat, 3.65 V full) ----
// Defaults of the batt_* settings, in millivolts. Order: sleep < low < warn <= resume.
#define BATT_ENABLED        1
#define BATT_WARN_MV        3100            // report only
#define BATT_LOW_MV         3000            // stop repeating and beaconing
#define BATT_SLEEP_MV       2800            // radio and processor sleep, supply checked every few seconds
#define BATT_RESUME_MV      3150            // back to normal at or above this

// ---- HOUSEKEEPING ---------------------------------------------
#define DEBUG_SERIAL        1           // 1: per-packet log (silence at run time with "set log off"); 0: remove it from the program. The command line is always on.
#define DEBUG_BAUD          115200
#define STATS_EVERY_MIN     10              // a counters line on the serial port
#define WATCHDOG_S          20              // restart if the loop stops for this long (a remote site has nobody to press reset)

// Keep the SWD debug port alive while the processor sleeps, so the ST-LINK can attach at any time.
// The main loop sits in WFI and batterySleep() uses STOP2; with this off the debug clock is gated in
// both, and the programmer can only get in during the moment after a reset (hold reset, release, and
// start the upload at once). Costs a little sleep current: a battery site can define it as 0
// (-D D3_DEBUG_SWD=0).
#ifndef D3_DEBUG_SWD
#define D3_DEBUG_SWD        1
#endif
