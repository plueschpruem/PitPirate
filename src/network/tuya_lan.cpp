// Tuya LAN v3.4 client for PitPirate
// Connects to the BBQ thermometer over TCP, negotiates a session key,
// queries data-points (DPs), and updates the shared jsonData/stringBattery.

#include "tuya_lan.h"
#include "shared_data.h"
#include "config.h"
#include "../fan_control.h"

#include <Arduino.h>
#include <WiFiClient.h>
#include <WiFiUDP.h>
#include <mbedtls/aes.h>
#include <mbedtls/md.h>

// ===== Config ===============================================================

static const uint16_t TUYA_PORT        = 6668;
static const uint32_t POLL_INTERVAL_MS = 30000UL;   // 30 s between polls
static const int      FRAME_BUF_SZ     = 512;
static const int      DEC_BUF_SZ       = 512;

// DP id → probe index mapping: DP_PROBES[i] maps to probe label (i+1)
static const int DP_PROBES[]  = {2, 3, 4, 5, 28, 29, 30};
static const int NUM_PROBES   = (int)(sizeof(DP_PROBES) / sizeof(DP_PROBES[0]));

// probe_state (DP 16) bit-to-probe-index mapping, confirmed by observation:
//   bit 0 → probe index 6 (ambient, DP 30, display "7")
//   bit N → probe index N-1 for N=1..5  (food probes 1-5)
//   bit 6 → NOT a probe; signals mis-insertion error (ambient probe in meat socket)
static const int PROBE_STATE_BIT_TO_IDX[6] = {6, 0, 1, 2, 3, 4};
static const int DP_BATTERY       = 1;
static const int DP_PROBE_STATE   = 16;   // raw byte bitmask — bit N → probe N+1 inserted
static const int DP_COOK_ALARM    = 23;   // bitmap: p1-p7 target/min/max/remove flags
static const int DP_DEVICE_ALARM  = 106;  // bitmap: device_charge | device_temp_h | device_temp_l
static const int DP_DEVICE_ON     = 107;  // bool: true=on, false=off

// Raw Tuya value ≤ this means "no probe connected"
static const int DP_DISCONNECTED = -200;

// Sentinel returned by dp_int() when the key is absent from the JSON
static const int DP_ABSENT = -10000;

// ===== Session state (persistent across polls) =====================================

static WiFiClient    tuya_client;
static uint8_t       s_skey[16]        = {};          // negotiated session key
static uint32_t      s_seqno           = 1;           // frame sequence counter
static bool          s_connected          = false;  // TCP + session established
static unsigned long s_last_query_ms      = 0;       // millis() of last {} query
static unsigned long s_last_connect_ms    = 0;       // millis() of last connect attempt
static unsigned long s_last_heartbeat_ms  = 0;       // millis() of last heartbeat

static const uint32_t RECONNECT_BACKOFF_MS  = 10000UL;
static const uint32_t HEARTBEAT_INTERVAL_MS = 10000UL;  // keepalive so device doesn't close TCP

// ===== Persistent device values (merged across polls + push events) ===========
// Maintained so that a push frame containing only one DP doesn't wipe others.

static int s_temps[7]    = {-10000,-10000,-10000,-10000,-10000,-10000,-10000};
static int s_battery     = -10000;
static int s_probe_state = -10000;
static int s_cook_alarm  = -10000;
static int s_dev_alarm   = -10000;
static int s_dev_on      = -10000;
static bool s_probe_ins[7]  = {};   // per-probe insertion decoded from probe_state bits 0-5
static bool s_misinsert     = false; // true when probe_state bit 6 is set (ambient probe in meat socket)

// ===== Runtime device config (loaded from NVS at init, falls back to config.h) =======

static char s_runtime_ip[64]  = {};
static char s_runtime_id[32]  = {};
static char s_runtime_key[17] = {};

// Forward declaration — defined in the crypto helpers section below.
static int aes_dec_pkcs7(const uint8_t key[16],
                          const uint8_t* in, int inlen, uint8_t* out);

// ===== Broadcast discovery (UDP 6667) =======================================

static WiFiUDP s_discover_udp;
static bool    s_discover_listening = false;

// Candidate IP from a broadcast packet that has not yet been verified.
// Empty string means no pending candidate.
// The IP is only promoted to s_runtime_ip (and saved to NVS) after a
// successful session handshake confirms the HMAC — i.e. the device actually
// holds our local key, proving it is the right device.
static char s_candidate_ip[20] = {};

// Fixed Tuya UDP broadcast decrypt key: md5("yGAdlopoPVldABfn")
// This is a well-known Tuya constant — NOT the device's local_key.
static uint8_t s_udp_key[16] = {};

// Opens the UDP socket that receives Tuya broadcast announcements.
// The NC01 (and other Tuya LAN devices) broadcast a frame with the
// 00 00 55 AA magic prefix from the device's own IP to 255.255.255.255:6667
// roughly every 20 seconds.  The sender IP is staged as a candidate;
// it is only committed to NVS after a successful handshake.
static void startDiscoverUdp()
{
    // Derive the fixed Tuya UDP broadcast decrypt key: md5("yGAdlopoPVldABfn")
    const mbedtls_md_info_t* md5_info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
    mbedtls_md(md5_info, (const uint8_t*)"yGAdlopoPVldABfn", 16, s_udp_key);

    if (s_discover_udp.begin(6667)) {
        s_discover_listening = true;
        Serial.println("[Tuya] UDP discovery listener on port 6667");
    } else {
        Serial.println("[Tuya] UDP discovery listener failed");
    }
}

// Reads one pending UDP packet from the discovery socket.
// Two-level identification:
//  1. Fast path — decrypt the broadcast payload (AES-128-ECB, local_key, offset 20)
//     and search for our device_id in the plaintext.  If found the sender is
//     definitively our device: save to NVS immediately without a TCP handshake.
//  2. Fallback  — if decryption produces no match (wrong device, different firmware
//     version, or key mismatch), stage the IP as a candidate.  tuyaLanLoop() will
//     then verify it via the TCP session-key handshake before updating NVS.
static void checkBroadcast()
{
    if (!s_discover_listening) return;

    int pktSize = s_discover_udp.parsePacket();
    if (pktSize <= 0) return;

    // Read the full packet so we can attempt payload decryption.
    uint8_t pkt[256];
    int n = s_discover_udp.read(pkt, sizeof(pkt) - 1);
    s_discover_udp.flush();
    if (n < 16) return;

    // Require the Tuya wire magic: 00 00 55 AA
    if (pkt[0] != 0x00 || pkt[1] != 0x00 || pkt[2] != 0x55 || pkt[3] != 0xAA) return;

    char sender_str[20];
    IPAddress sender = s_discover_udp.remoteIP();
    snprintf(sender_str, sizeof(sender_str), "%d.%d.%d.%d",
             sender[0], sender[1], sender[2], sender[3]);

    if (strcmp(sender_str, s_runtime_ip) == 0) return;   // already confirmed

    if (s_connected) {
        Serial.printf("[Tuya] broadcast from %s (ignored while connected)\n", sender_str);
        return;
    }

    // ── Fast path: decrypt payload and match device_id ────────────────────
    // Tuya v3.3/3.4 UDP broadcasts are AES-128-ECB encrypted with a fixed key:
    //   udpkey = md5("yGAdlopoPVldABfn")  (NOT the device's local_key)
    // Format: [16-byte header][4-byte retcode][N bytes AES-ECB][4 CRC][4 suffix]
    // The rounding below automatically strips the CRC+suffix tail.
    const int ENC_OFFSET = 20;   // 16-byte header + 4-byte retcode
    if (n > ENC_OFFSET) {
        int enc_len = ((n - ENC_OFFSET) / 16) * 16;   // trim to AES block boundary (strips CRC+suffix tail)
        if (enc_len >= 16) {
            uint8_t decbuf[256] = {};
            aes_dec_pkcs7(s_udp_key, pkt + ENC_OFFSET, enc_len, decbuf);
            decbuf[enc_len] = '\0';   // safe: pkt is 256, dec can't exceed enc_len
            if (strstr((const char*)decbuf, s_runtime_id)) {
                Serial.printf("[Tuya] broadcast from %s — device_id matched, saving IP\n",
                              sender_str);
                Serial.printf("[Tuya] broadcast payload: %.*s\n", enc_len, (const char*)decbuf);
                strlcpy(s_runtime_ip, sender_str, sizeof(s_runtime_ip));
                preferences.putString("tuya_ip", sender_str);
                s_candidate_ip[0] = '\0';
                s_last_connect_ms  = 0;
                return;
            }
        }
    }

    // ── Fallback: stage as unverified candidate ────────────────────────────
    if (strcmp(sender_str, s_candidate_ip) == 0) return;   // already staged
    Serial.printf("[Tuya] broadcast from %s — no device_id match, staging as candidate\n",
                  sender_str);
    strlcpy(s_candidate_ip, sender_str, sizeof(s_candidate_ip));
    s_last_connect_ms = 0;
}

// ===== Crypto helpers =======================================================

// AES-128-ECB encrypt with PKCS7 padding (always adds 1–16 pad bytes).
// Returns output length (>0) on success, -1 on error.
// `out` must hold at least ((inlen/16)+1)*16 bytes.
// AES-128-ECB encrypt with PKCS7 padding (always adds 1–16 pad bytes).
// @param key    16-byte AES key
// @param in     Plaintext input buffer
// @param inlen  Plaintext length in bytes (max 240)
// @param out    Output buffer; must hold at least ((inlen/16)+1)*16 bytes
// @return       Ciphertext length on success, -1 if inlen is out of range
static int aes_enc_pkcs7(const uint8_t key[16],
                          const uint8_t* in, int inlen, uint8_t* out)
{
    if (inlen < 0 || inlen > 240) return -1;
    int outlen = ((inlen / 16) + 1) * 16;   // always one extra block
    uint8_t buf[256];
    memcpy(buf, in, inlen);
    uint8_t pad = (uint8_t)(outlen - inlen);
    memset(buf + inlen, pad, pad);

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_enc(&ctx, key, 128);
    for (int i = 0; i < outlen; i += 16)
        mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, buf + i, out + i);
    mbedtls_aes_free(&ctx);
    return outlen;
}

// AES-128-ECB decrypt and strip PKCS7 padding.
// Returns plaintext length (≥0) on success, -1 on error.
// AES-128-ECB decrypt and strip PKCS7 padding.
// @param key    16-byte AES key
// @param in     Ciphertext buffer (must be a multiple of 16 bytes)
// @param inlen  Ciphertext length in bytes
// @param out    Output buffer; must hold at least inlen bytes
// @return       Plaintext length on success (>= 0), -1 on invalid input
static int aes_dec_pkcs7(const uint8_t key[16],
                          const uint8_t* in, int inlen, uint8_t* out)
{
    if (inlen <= 0 || inlen % 16 != 0) return -1;
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_dec(&ctx, key, 128);
    for (int i = 0; i < inlen; i += 16)
        mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_DECRYPT, in + i, out + i);
    mbedtls_aes_free(&ctx);

    uint8_t pad = out[inlen - 1];
    if (pad < 1 || pad > 16) return inlen;   // no recognised padding → keep all
    return inlen - (int)pad;
}

// AES-128-ECB encrypt exactly one 16-byte block (no padding).
// AES-128-ECB encrypt exactly one 16-byte block (no padding).
// @param key  16-byte AES key
// @param in   16-byte plaintext input block
// @param out  16-byte ciphertext output block
static void aes_enc16(const uint8_t key[16],
                       const uint8_t in[16], uint8_t out[16])
{
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_enc(&ctx, key, 128);
    mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, in, out);
    mbedtls_aes_free(&ctx);
}

// HMAC-SHA256(key, klen, data, dlen) → 32-byte digest into `out`.
// Computes HMAC-SHA256 of the given data with the given key.
// @param key   HMAC key bytes
// @param klen  Key length in bytes
// @param data  Message bytes
// @param dlen  Message length in bytes
// @param out   32-byte buffer that receives the digest
static void hmac256(const uint8_t* key, int klen,
                     const uint8_t* data, int dlen, uint8_t out[32])
{
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&ctx, key, (size_t)klen);
    mbedtls_md_hmac_update(&ctx, data, (size_t)dlen);
    mbedtls_md_hmac_finish(&ctx, out);
    mbedtls_md_free(&ctx);
}

// ===== Frame helpers ========================================================

// Build an outgoing v3.4 frame into `buf`.
//
// Frame layout (sent from client → device):
//   [00 00 55 AA : 4][seqno : 4BE][cmd : 4BE][length : 4BE]
//   [enc_payload : N]
//   [HMAC-SHA256(hmac_key, header+payload) : 32][00 00 AA 55 : 4]
//   length = N + 36
//
// Returns total frame length, or -1 if `buf` is too small.
// Builds an outgoing Tuya v3.4 frame into `buf`.
// Frame layout: [prefix:4][seqno:4BE][cmd:4BE][length:4BE][enc_payload:N][HMAC-SHA256:32][suffix:4]
// The HMAC covers the header (16 bytes) + enc_payload.
// @param buf      Output buffer
// @param bufsize  Size of output buffer in bytes
// @param sn       Frame sequence number (monotonically increasing)
// @param cmd      Tuya protocol command code (e.g. 3=NEG_START, 9=HEARTBEAT, 16=QUERY)
// @param enc      Already-encrypted payload bytes (may be null if enclen==0)
// @param enclen   Length of encrypted payload in bytes
// @param hmac_key 16-byte key used to compute the frame HMAC
// @return         Total frame length in bytes, or -1 if buf is too small
static int build_frame(uint8_t* buf, int bufsize,
                        uint32_t sn, uint32_t cmd,
                        const uint8_t* enc, int enclen,
                        const uint8_t hmac_key[16])
{
    int total = 16 + enclen + 36;
    if (total > bufsize) return -1;

    buf[0] = 0x00; buf[1] = 0x00; buf[2] = 0x55; buf[3] = 0xAA;
    buf[4] = (uint8_t)(sn >> 24);  buf[5] = (uint8_t)(sn >> 16);
    buf[6] = (uint8_t)(sn >>  8);  buf[7] = (uint8_t)(sn);
    buf[8] = (uint8_t)(cmd >> 24); buf[9] = (uint8_t)(cmd >> 16);
    buf[10]= (uint8_t)(cmd >>  8); buf[11]= (uint8_t)(cmd);
    uint32_t len = (uint32_t)enclen + 36;
    buf[12]= (uint8_t)(len >> 24); buf[13]= (uint8_t)(len >> 16);
    buf[14]= (uint8_t)(len >>  8); buf[15]= (uint8_t)(len);

    if (enclen > 0) memcpy(buf + 16, enc, enclen);

    uint8_t hmac[32];
    hmac256(hmac_key, 16, buf, 16 + enclen, hmac);
    memcpy(buf + 16 + enclen, hmac, 32);

    buf[16 + enclen + 32] = 0x00;
    buf[16 + enclen + 33] = 0x00;
    buf[16 + enclen + 34] = 0xAA;
    buf[16 + enclen + 35] = 0x55;

    return total;
}

// Read one complete frame from the device into `buf`.
//
// Received frame layout:
//   [header : 16][retcode : 4][enc_payload : N][HMAC : 32][suffix : 4]
//   header.length = 4 + N + 36   →   N = header.length − 40
//
// On success sets out_cmd, out_enc (pointer into buf+20), out_enc_len (N).
// Returns false on timeout or protocol error.
// Reads one complete Tuya v3.4 frame from the TCP client into `buf`.
// Received layout: [header:16][retcode:4][enc_payload:N][HMAC:32][suffix:4]
// enc_payload length N = header.length − 40.
// @param c            TCP client to read from
// @param buf          Working buffer (must be >= bufsize bytes)
// @param bufsize      Size of buf in bytes
// @param out_cmd      Set to the command field from the received frame header
// @param out_enc      Set to a pointer into buf at the start of enc_payload (or null)
// @param out_enc_len  Set to the enc_payload length in bytes (0 if absent)
// @param timeout_ms   Maximum wait time in milliseconds (default 4000)
// @return             true on success, false on timeout or protocol error
static bool recv_frame(WiFiClient& c,
                        uint8_t* buf, int bufsize,
                        uint32_t& out_cmd,
                        uint8_t*& out_enc, int& out_enc_len,
                        uint32_t timeout_ms = 4000)
{
    unsigned long start = millis();
    int got = 0;
    while (got < 16) {
        if ((unsigned long)(millis() - start) >= timeout_ms) {
            Serial.printf("[Tuya] header timeout (got %d/16)\n", got);
            return false;
        }
        if (c.available()) buf[got++] = (uint8_t)c.read();
        else               delay(1);
    }

    // Validate prefix
    if (buf[0] != 0x00 || buf[1] != 0x00 || buf[2] != 0x55 || buf[3] != 0xAA) {
        Serial.printf("[Tuya] bad prefix %02X%02X%02X%02X\n",
                      buf[0], buf[1], buf[2], buf[3]);
        return false;
    }

    out_cmd = ((uint32_t)buf[8]  << 24) | ((uint32_t)buf[9]  << 16)
            | ((uint32_t)buf[10] <<  8) |  (uint32_t)buf[11];
    uint32_t length = ((uint32_t)buf[12] << 24) | ((uint32_t)buf[13] << 16)
                    | ((uint32_t)buf[14] <<  8) |  (uint32_t)buf[15];

    if (length < 40u || (int)(16u + length) > bufsize) {
        Serial.printf("[Tuya] bad length %u\n", (unsigned)length);
        return false;
    }

    // Read body
    got = 0;
    start = millis();
    while (got < (int)length) {
        if ((unsigned long)(millis() - start) >= timeout_ms) {
            Serial.printf("[Tuya] body timeout (%d/%u)\n", got, (unsigned)length);
            return false;
        }
        if (c.available()) buf[16 + got++] = (uint8_t)c.read();
        else               delay(1);
    }

    // enc_payload sits after header(16) + retcode(4)
    out_enc     = buf + 20;
    out_enc_len = (int)length - 40;   // strip retcode(4) + HMAC(32) + suffix(4)
    if (out_enc_len <= 0) { out_enc = nullptr; out_enc_len = 0; }

    return true;
}

// ===== JSON DP parser =======================================================

// Return the integer value of DP `dp` from a Tuya JSON string
// e.g. dp_int(json, 2) looks for "\"2\":NNN" and returns NNN.
// Returns DP_ABSENT if the key is missing or if the value is a string/bool.
// Extracts the integer value of a numeric DP from a Tuya DPS JSON string.
// Looks for the pattern "\"<dp>\":<integer>" and parses the number.
// @param json  Null-terminated DPS JSON string (e.g. {"2":950,"3":820})
// @param dp    Tuya data-point ID to look up
// @return      Parsed integer on success; DP_ABSENT if the key is missing
//              or the value is a string/bool rather than an integer.
static int dp_int(const char* json, int dp)
{
    char key[14];
    snprintf(key, sizeof(key), "\"%d\":", dp);
    const char* p = strstr(json, key);
    if (!p) return DP_ABSENT;
    p += strlen(key);
    if (*p == '"' || *p == 't' || *p == 'f') return DP_ABSENT;   // non-integer
    return (int)strtol(p, nullptr, 10);
}

// Decode a Tuya LAN raw-type DP that encodes a single byte as two base64 chars
// (without the trailing "==" padding that the cloud API includes).
// e.g. probe_state: "16":"Aw"  → A=0, w=48 → (0<<2)|(48>>4) = 0x03
// Returns the decoded byte, or DP_ABSENT if the key is missing / not a string.
// Decodes a raw-type Tuya DP that represents a single byte as two base64 characters.
// Used to decode probe_state (DP 16), e.g. "16":"Aw" → 0x03.
// @param json  Null-terminated DPS JSON string
// @param dp    Tuya data-point ID to look up
// @return      Decoded byte value; DP_ABSENT if the key is missing or not a base64 string
static int dp_raw_base64_byte(const char* json, int dp)
{
    char key[14];
    snprintf(key, sizeof(key), "\"%d\":\"", dp);
    const char* p = strstr(json, key);
    if (!p) return DP_ABSENT;
    p += strlen(key);
    if (*p == '"' || *p == '}' || *p == '\0') return DP_ABSENT;

    // Decode one base64 character to its 6-bit value
    auto b64val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    int c1 = b64val(p[0]);
    if (c1 < 0) return DP_ABSENT;
    int c2 = (p[1] != '=' && p[1] != '"' && p[1] != '\0') ? b64val(p[1]) : 0;
    if (c2 < 0) c2 = 0;
    return (c1 << 2) | (c2 >> 4);
}

// Return 1 (true), 0 (false), or DP_ABSENT for a boolean DP.
// Extracts the boolean value of a DP from a Tuya DPS JSON string.
// @param json  Null-terminated DPS JSON string
// @param dp    Tuya data-point ID to look up
// @return      1 if true, 0 if false, DP_ABSENT if the key is missing or not a boolean
static int dp_bool(const char* json, int dp)
{
    char key[14];
    snprintf(key, sizeof(key), "\"%d\":", dp);
    const char* p = strstr(json, key);
    if (!p) return DP_ABSENT;
    p += strlen(key);
    if (strncmp(p, "true",  4) == 0) return 1;
    if (strncmp(p, "false", 5) == 0) return 0;
    return DP_ABSENT;
}

// ===== Merge incoming DPs into persistent state and re-render jsonData ========

// Merges an incoming Tuya DPS JSON update into the persistent per-session device state,
// then rebuilds the shared `jsonData` string consumed by GET /data.
// Only DPs present in the JSON are updated; absent DPs keep their previous values.
// Decodes probe_state bits to determine which probes are physically inserted.
// Re-renders temperatures as floats (raw Tuya tenths-of-a-degree ÷ 10).
// @param json  Null-terminated DPS JSON string (e.g. from cmd=8 push or cmd=16 response)
static void applyDpsJson(const char* json)
{
    for (int i = 0; i < NUM_PROBES; i++) {
        int v = dp_int(json, DP_PROBES[i]);
        if (v != DP_ABSENT) s_temps[i] = v;
    }
    { int v = dp_int(json, DP_BATTERY);       if (v != DP_ABSENT && v >= 0 && v <= 100) s_battery = v; }
    { int v = dp_raw_base64_byte(json, DP_PROBE_STATE); if (v == DP_ABSENT) v = dp_int(json, DP_PROBE_STATE); if (v != DP_ABSENT) {
        s_probe_state = v;
        for (int bit = 0; bit < 6; bit++)
            s_probe_ins[PROBE_STATE_BIT_TO_IDX[bit]] = (v >> bit) & 1;
        s_misinsert = (v & 0x40) != 0;   // bit 6 = ambient probe in wrong socket
    } }
    { int v = dp_int(json, DP_COOK_ALARM);    if (v != DP_ABSENT) s_cook_alarm = v; }
    { int v = dp_int(json, DP_DEVICE_ALARM);  if (v != DP_ABSENT) s_dev_alarm  = v; }
    { int v = dp_bool(json, DP_DEVICE_ON);    if (v != DP_ABSENT) s_dev_on     = v; }

    // Re-render full jsonData from merged state
    // Format: [{"probes":{"1":25.0,...},"battery":100,...}]
    String jd = "[{\"probes\":{";
    bool firstProbe = true;
    for (int i = 0; i < NUM_PROBES; i++) {
        // Skip if no reading at all, or if device sent the "no probe" sentinel (-200)
        if (s_temps[i] <= DP_DISCONNECTED) continue;
        // Use probe_state bits when available; fall back to temp threshold
        bool connected = (s_probe_state != DP_ABSENT)
                       ? s_probe_ins[i]
                       : true;  // temp already filtered above by > DP_DISCONNECTED
        if (!connected) continue;
        if (!firstProbe) jd += ',';
        jd += '"'; jd += String(i + 1); jd += "\":";
        jd += String(s_temps[i] / 10.0f, 1);
        firstProbe = false;
    }
    jd += '}';
    if (s_battery != DP_ABSENT)     { jd += ",\"battery\":";      jd += String(s_battery); }
    if (s_probe_state != DP_ABSENT) { jd += ",\"probe_state\":";  jd += String(s_probe_state); }
    if (s_cook_alarm != DP_ABSENT)  { jd += ",\"cook_alarm\":";   jd += String(s_cook_alarm); }
    if (s_dev_alarm != DP_ABSENT)   { jd += ",\"device_alarm\":"; jd += String(s_dev_alarm); }
    if (s_dev_on != DP_ABSENT)      { jd += ",\"device_on\":";    jd += (s_dev_on ? '1' : '0'); }
    if (s_misinsert)                 { jd += ",\"error\":1"; }
    { jd += ",\"fan_pct\":"; jd += String(fanGetPercent()); }

    jd += "}]";

    jsonData = jd;
}

// ===== Session management ====================================================

// Non-blocking check: if a frame is waiting, read it, decrypt, and apply.
// Returns true if a frame was processed.
// Non-blocking check: reads and processes one pending Tuya frame if available.
// Decrypts the payload using the session key (s_skey) and calls applyDpsJson().
// Handles cmd=8 (unsolicited push) and cmd=16 (query response); ignores others.
// @return  true if a frame was successfully read (caller may call again to drain burst),
//          false if no data was available or the frame could not be parsed
static bool tryRecvAndApply()
{
    if (!tuya_client.available()) return false;

    uint8_t rxbuf[FRAME_BUF_SZ];
    uint32_t rx_cmd; uint8_t* enc_ptr; int enc_len;
    if (!recv_frame(tuya_client, rxbuf, sizeof(rxbuf),
                    rx_cmd, enc_ptr, enc_len, 2000))
        return false;

    // cmd=8: unsolicited status push; cmd=16: query response
    if ((rx_cmd != 8 && rx_cmd != 16) || enc_len <= 0) return true;

    uint8_t decbuf[DEC_BUF_SZ];
    int dec_len = aes_dec_pkcs7(s_skey, enc_ptr, enc_len, decbuf);
    if (dec_len <= 0) return true;
    decbuf[dec_len] = '\0';

    // v3.4 cmd=8 push payloads have a 15-byte prefix ("3.4" + 12 header bytes)
    // before the JSON. Seek to the first '{' to handle any prefix length.
    const char* json_start = (const char*)memchr(decbuf, '{', (size_t)dec_len);
    if (!json_start) return true;

    applyDpsJson(json_start);
    return true;
}

// Connect + negotiate v3.4 session; fills s_skey / s_seqno.
// Establishes a TCP connection to the Tuya device and performs the v3.4
// session-key negotiation (cmd=3 → cmd=4 → cmd=5 exchange).
// On success, fills `s_skey` with the derived session key and drains any
// initial unsolicited push frames (raw DPs such as probe_state arrive here).
// @return  true if TCP connected and session key negotiated successfully;
//          false on TCP failure, unexpected command, or HMAC mismatch
static bool tuyaConnect()
{
    const uint8_t* rkey = (const uint8_t*)s_runtime_key;

    if (!tuya_client.connect(s_runtime_ip, TUYA_PORT)) {
        Serial.println("[Tuya] TCP connect failed");
        return false;
    }

    s_seqno = 1;

    // ── Step 1: SESS_KEY_NEG_START (cmd=3) ──────────────────────────────
    static const uint8_t local_nonce[16] = {
        '0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f'
    };
    uint8_t enc_nonce[32];
    if (aes_enc_pkcs7(rkey, local_nonce, 16, enc_nonce) != 32) {
        tuya_client.stop(); return false;
    }
    uint8_t frame[FRAME_BUF_SZ];
    int flen = build_frame(frame, sizeof(frame), s_seqno++, 3,
                            enc_nonce, 32, rkey);
    tuya_client.write(frame, flen);

    // ── Step 2: SESS_KEY_NEG_RESP (cmd=4) ───────────────────────────────
    uint8_t rxbuf[FRAME_BUF_SZ];
    uint32_t rx_cmd; uint8_t* enc_ptr; int enc_len;
    if (!recv_frame(tuya_client, rxbuf, sizeof(rxbuf), rx_cmd, enc_ptr, enc_len)) {
        Serial.println("[Tuya] no response to cmd=3");
        tuya_client.stop(); return false;
    }
    if (rx_cmd != 4) {
        Serial.printf("[Tuya] expected cmd=4, got %u\n", (unsigned)rx_cmd);
        tuya_client.stop(); return false;
    }
    uint8_t decbuf[DEC_BUF_SZ];
    int dec_len = aes_dec_pkcs7(rkey, enc_ptr, enc_len, decbuf);
    if (dec_len < 48) {
        Serial.printf("[Tuya] cmd=4 payload too short (%d)\n", dec_len);
        tuya_client.stop(); return false;
    }
    uint8_t remote_nonce[16];
    memcpy(remote_nonce, decbuf, 16);
    uint8_t expected_hmac[32];
    hmac256(rkey, 16, local_nonce, 16, expected_hmac);
    if (memcmp(expected_hmac, decbuf + 16, 32) != 0) {
        Serial.println("[Tuya] HMAC verify failed — check TUYA_LOCAL_KEY");
        tuya_client.stop(); return false;
    }

    // ── Step 3: SESS_KEY_NEG_FINISH (cmd=5) ─────────────────────────────
    uint8_t finish_raw[32];
    hmac256(rkey, 16, remote_nonce, 16, finish_raw);
    uint8_t finish_enc[48];
    if (aes_enc_pkcs7(rkey, finish_raw, 32, finish_enc) != 48) {
        tuya_client.stop(); return false;
    }
    flen = build_frame(frame, sizeof(frame), s_seqno++, 5,
                        finish_enc, 48, rkey);
    tuya_client.write(frame, flen);

    // ── Derive session key ───────────────────────────────────────────────
    uint8_t xor_key[16];
    for (int i = 0; i < 16; i++) xor_key[i] = local_nonce[i] ^ remote_nonce[i];
    aes_enc16(rkey, xor_key, s_skey);

    // Wait for the device's post-handshake unsolicited push(es).
    // Raw DPs like probe_state (DP 16) only arrive here — NOT in {} query responses.
    // Use a silence-timeout so we catch slow devices without blocking indefinitely.
    // Hard cap at 5 s: if the device keeps sending frames back-to-back the
    // silence timer would never expire and the loop would hang forever.
    {
        unsigned long drain_start   = millis();
        unsigned long silence_since = drain_start;
        while ((millis() - silence_since) < 600) {
            if ((millis() - drain_start) >= 5000) break;   // safety cap
            if (tryRecvAndApply()) silence_since = millis();
            else                  delay(10);
        }
    }

    Serial.println("[Tuya] connected");
    return true;
}

// Send a heartbeat (cmd=9) and consume the echo (cmd=9).
// Returns false if the write or echo fails (session is dead).
static bool tuyaSendHeartbeat()
{
    // Heartbeat payload: AES(skey, "{}") — same 16-byte block as query
    uint8_t hb_enc[16];
    const uint8_t hb_payload[] = {'{', '}'};
    if (aes_enc_pkcs7(s_skey, hb_payload, 2, hb_enc) != 16) return false;

    uint8_t frame[FRAME_BUF_SZ];
    int flen = build_frame(frame, sizeof(frame), s_seqno++, 9,
                            hb_enc, 16, s_skey);
    if (tuya_client.write(frame, flen) != (size_t)flen) return false;

    // The device echoes a cmd=9 frame back; drain it non-blockingly
    // (tryRecvAndApply already tolerates unknown commands, so just let
    //  the next loop iteration drain it — no need to block here).
    return true;
}

// Send a {} query on the existing session and wait for the dp response.
static bool tuyaSendQuery()
{
    uint8_t dp_enc[16];
    const uint8_t dp_payload[] = {'{', '}'};
    if (aes_enc_pkcs7(s_skey, dp_payload, 2, dp_enc) != 16) return false;

    uint8_t frame[FRAME_BUF_SZ];
    int flen = build_frame(frame, sizeof(frame), s_seqno++, 16,
                            dp_enc, 16, s_skey);
    tuya_client.write(frame, flen);

    uint8_t rxbuf[FRAME_BUF_SZ];
    uint32_t rx_cmd; uint8_t* enc_ptr; int enc_len;
    if (!recv_frame(tuya_client, rxbuf, sizeof(rxbuf),
                    rx_cmd, enc_ptr, enc_len, 5000)) {
        Serial.println("[Tuya] no DP query response");
        return false;
    }
    if (enc_len <= 0) return true;

    uint8_t decbuf[DEC_BUF_SZ];
    int dec_len = aes_dec_pkcs7(s_skey, enc_ptr, enc_len, decbuf);
    if (dec_len <= 0) return false;
    decbuf[dec_len] = '\0';

    applyDpsJson((const char*)decbuf);
    return true;
}

// ===== Public API ===========================================================

// Initialises the Tuya LAN client.
// Loads device IP, ID, and local key from NVS (if previously saved) falling
// back to compile-time TUYA_DEVICE_IP/ID/LOCAL_KEY values from config.h.
// Resets connection state so that tuyaLanLoop() will attempt to connect.
// Must be called once from setup() after WiFi is established.
void tuyaLanInit()
{
    // Load from NVS, fall back to compile-time config.h defaults.
    // isKey() guard avoids spurious E-level log from Preferences when the key
    // has never been written (i.e. first boot before any settings are saved).
    String ip  = preferences.isKey("tuya_ip")  ? preferences.getString("tuya_ip")  : String(TUYA_DEVICE_IP);
    String id  = preferences.isKey("tuya_id")  ? preferences.getString("tuya_id")  : String(TUYA_DEVICE_ID);
    String key = preferences.isKey("tuya_key") ? preferences.getString("tuya_key") : String(TUYA_LOCAL_KEY);
    strlcpy(s_runtime_ip,  ip.c_str(),  sizeof(s_runtime_ip));
    strlcpy(s_runtime_id,  id.c_str(),  sizeof(s_runtime_id));
    strlcpy(s_runtime_key, key.c_str(), sizeof(s_runtime_key));

    s_connected         = false;
    s_last_query_ms     = 0;
    s_last_connect_ms   = 0;
    s_last_heartbeat_ms = 0;

    startDiscoverUdp();
}

// Main Tuya LAN state machine; must be called from loop() every iteration.
// Manages the TCP connection lifecycle:
//   - Reconnects (with 10 s backoff) if not connected or connection is lost
//   - Drains unsolicited push frames (probe values, alarms, etc.)
//   - Sends a keepalive heartbeat (cmd=9) every 10 s
//   - Sends a {} DP query (cmd=16) every 30 s or when requestConnection is set
// Updates the shared `jsonData` string on each successful DP response.
void tuyaLanLoop()
{
    // Check for broadcast IP announcements from the Tuya device first, so
    // that a freshly discovered IP is available before the connect block runs.
    checkBroadcast();

    unsigned long now = millis();

    // ── Maintain persistent connection ───────────────────────────────────
    if (!s_connected || !tuya_client.connected()) {
        if (s_connected) {
            Serial.println("[Tuya] connection lost, reconnecting");
            tuya_client.stop();
            s_connected = false;
        }
        if ((now - s_last_connect_ms) < RECONNECT_BACKOFF_MS) return;
        s_last_connect_ms = now;
        // Reset all device state before reconnecting so the post-handshake push
        // fills everything fresh and stale readings from the old session can't bleed through.
        for (int i = 0; i < 7; i++) s_temps[i] = DP_ABSENT;
        s_battery     = DP_ABSENT;
        s_probe_state = DP_ABSENT;
        s_cook_alarm  = DP_ABSENT;
        s_dev_alarm   = DP_ABSENT;
        s_dev_on      = DP_ABSENT;
        memset(s_probe_ins, 0, sizeof(s_probe_ins));
        s_misinsert   = false;
        // If a candidate IP was discovered via broadcast, try it first.
        // Revert to the confirmed runtime IP on failure so we don't persist
        // a wrong device's address.
        const char* prev_ip = nullptr;
        char prev_ip_buf[64] = {};
        if (s_candidate_ip[0] != '\0' && strcmp(s_candidate_ip, s_runtime_ip) != 0) {
            strlcpy(prev_ip_buf, s_runtime_ip, sizeof(prev_ip_buf));
            prev_ip = prev_ip_buf;
            strlcpy(s_runtime_ip, s_candidate_ip, sizeof(s_runtime_ip));
        }
        if (!tuyaConnect()) {
            if (prev_ip) {
                // Handshake failed — this was not our device; revert
                Serial.printf("[Tuya] candidate %s rejected (wrong device or key), reverting\n",
                              s_runtime_ip);
                strlcpy(s_runtime_ip, prev_ip, sizeof(s_runtime_ip));
            }
            s_candidate_ip[0] = '\0';  // clear so we try again on next broadcast
            jsonData = "[{\"error\":0}]";
            return;
        }
        // Handshake succeeded — HMAC verified, device confirmed.
        // Promote candidate to the persistent runtime IP if it differs.
        if (prev_ip && strcmp(s_runtime_ip, prev_ip) != 0) {
            Serial.printf("[Tuya] device confirmed at %s — saving to NVS\n", s_runtime_ip);
            preferences.putString("tuya_ip", s_runtime_ip);
        }
        s_candidate_ip[0] = '\0';
        s_connected      = true;
        s_last_query_ms  = 0;   // force immediate poll after connect
        s_last_heartbeat_ms = now;
    }

    // ── Drain any unsolicited push frames (probe_state, alarms, etc.) ────
    while (tryRecvAndApply()) {}

    // ── Heartbeat every 10 s to keep the TCP connection alive ─────────────
    if ((now - s_last_heartbeat_ms) >= HEARTBEAT_INTERVAL_MS) {
        s_last_heartbeat_ms = now;
        if (!tuyaSendHeartbeat()) {
            tuya_client.stop();
            s_connected = false;
            return;
        }
    }

    // ── Periodic {} query ─────────────────────────────────────────────────
    bool do_query = requestConnection
                 || s_last_query_ms == 0
                 || (now - s_last_query_ms) >= POLL_INTERVAL_MS;
    if (!do_query) return;

    requestConnection = false;
    if (tuyaSendQuery()) {
        // Drain the response burst.
        while (tryRecvAndApply()) {}
        s_last_query_ms = millis();
    } else {
        tuya_client.stop();
        s_connected = false;
        jsonData    = "[{\"error\":0}]";
    }
}

// Updates the Tuya device connection settings and forces an immediate reconnect.
// Persists the new values to NVS and overwrites the runtime state so the next
// tuyaLanLoop() cycle uses the new credentials without a reboot.
// @param ip          LAN IP address of the Tuya device (null-terminated, max 63 chars)
// @param device_id   Tuya cloud device ID (null-terminated, max 31 chars)
// @param local_key   16-character local encryption key (null-terminated)
void tuyaApplyNewSettings(const char* ip, const char* device_id, const char* local_key)
{
    preferences.putString("tuya_ip",  ip);
    preferences.putString("tuya_id",  device_id);
    preferences.putString("tuya_key", local_key);
    strlcpy(s_runtime_ip,  ip,        sizeof(s_runtime_ip));
    strlcpy(s_runtime_id,  device_id, sizeof(s_runtime_id));
    strlcpy(s_runtime_key, local_key, sizeof(s_runtime_key));
    // Force immediate reconnect with new credentials
    tuya_client.stop();
    s_connected       = false;
    s_last_connect_ms = 0;
    Serial.println("[Tuya] new settings applied, reconnecting");
}

// Returns the current runtime Tuya connection settings.
// Reads from the in-memory runtime state (which mirrors NVS after init).
// @param ip_out   Output buffer for device IP (must be at least 64 bytes)
// @param id_out   Output buffer for device ID (must be at least 32 bytes)
// @param key_out  Output buffer for local key (must be at least 17 bytes)
void tuyaGetSettings(char ip_out[64], char id_out[32], char key_out[17])
{
    strlcpy(ip_out,  s_runtime_ip,  64);
    strlcpy(id_out,  s_runtime_id,  32);
    strlcpy(key_out, s_runtime_key, 17);
}
