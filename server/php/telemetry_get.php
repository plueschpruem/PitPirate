<?php
/**
 * PitPirate – chart data endpoint
 *
 * GET  /telemetry_get.php?probe=<id>&limit=<n>
 *        probe  – required, probe id (1–7) | 'all' | 'alarms'
 *        limit  – number of most-recent entries to return
 *                 (default 360 = 1 h at 10-s intervals)
 *
 * Returns a JSON array of { ts, temp } objects for the requested probe.
 * Authentication and DATA_FILE must match telemetry.php.
 */

define('DATA_FILE',    __DIR__ . '/pitpirate_log.ndjson');
define('ALARM_FILE',   __DIR__ . '/pitpirate_alarms.json');
define('MAX_ENTRIES',  8640);  // 24 h at 10-s intervals

// ── CORS / headers ────────────────────────────────────────────────────────────
header('Content-Type: application/json; charset=utf-8');
header('Access-Control-Allow-Origin: *');
header('Access-Control-Allow-Methods: GET, OPTIONS');

if ($_SERVER['REQUEST_METHOD'] === 'OPTIONS') {
    http_response_code(204);
    exit;
}

// No authentication required — read-only chart data endpoint.

if ($_SERVER['REQUEST_METHOD'] !== 'GET') {
    http_response_code(405);
    exit(json_encode(['error' => 'Method not allowed']));
}

// ── Input validation ──────────────────────────────────────────────────────────
$probe = isset($_GET['probe']) ? trim((string) $_GET['probe']) : '';
if ($probe === '') {
    http_response_code(400);
    exit(json_encode(['error' => 'probe parameter required']));
}

// probe=alarms → return the saved alarm limits (no auth required – read-only)
if ($probe === 'alarms') {
    if (!file_exists(ALARM_FILE)) {
        exit(json_encode((object)[]));
    }
    exit(file_get_contents(ALARM_FILE));
}

// probe=fan → return fan_pct history as [{ts, temp}] (temp = fan_pct %)
if ($probe === 'fan') {
    if (!file_exists(DATA_FILE)) {
        exit(json_encode([]));
    }
    $limit = isset($_GET['limit'])
           ? max(1, min((int) $_GET['limit'], MAX_ENTRIES))
           : 360;
    $since   = isset($_GET['since']) ? (int) $_GET['since'] : null;
    $samples = isset($_GET['samples']) ? max(1, min((int) $_GET['samples'], MAX_ENTRIES)) : 0;
    $lines   = file(DATA_FILE, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES);
    $result  = [];
    foreach ($lines as $line) {
        $entry = json_decode($line, true);
        if (!is_array($entry)) continue;
        if (empty($entry['ts'])) continue;
        $ts = (int) $entry['ts'];
        if ($since !== null && $ts <= $since) continue;
        if (!isset($entry['fan_pct'])) continue;
        $result[] = ['ts' => $ts, 'temp' => (int) $entry['fan_pct']];
    }
    if ($since === null) {
        $result = array_slice($result, -$limit);
    }
    if ($samples > 0 && count($result) > $samples) {
        $total   = count($result);
        $out     = [];
        for ($b = 0; $b < $samples; $b++) {
            $start = (int) floor($b         * $total / $samples);
            $end   = (int) floor(($b + 1)   * $total / $samples);
            if ($end <= $start) continue;
            $slice   = array_slice($result, $start, $end - $start);
            $avgTemp = array_sum(array_column($slice, 'temp')) / count($slice);
            $midTs   = $slice[(int) floor(count($slice) / 2)]['ts'];
            $out[]   = ['ts' => $midTs, 'temp' => (int) round($avgTemp)];
        }
        $result = $out;
    }
    exit(json_encode(array_values($result)));
}

// probe=all → return the most-recent full log entry (RawData format for live display)
if ($probe === 'all') {
    if (!file_exists(DATA_FILE)) {
        exit(json_encode(['probes' => (object)[]]));
    }
    $lines = file(DATA_FILE, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES);
    for ($i = count($lines) - 1; $i >= 0; $i--) {
        $entry = json_decode($lines[$i], true);
        if (!is_array($entry) || empty($entry['probes'])) continue;
        if (empty($entry['ts'])) continue;    // skip ts=0 (NTP not synced)
        exit(json_encode($entry));
    }
    exit(json_encode(['probes' => (object)[]]));
}

// Only allow probe IDs 1–7
if (!preg_match('/^[1-7]$/', $probe)) {
    http_response_code(400);
    exit(json_encode(['error' => 'Invalid probe id']));
}

$limit = isset($_GET['limit'])
       ? max(1, min((int) $_GET['limit'], MAX_ENTRIES))
       : 360;

// Optional: only return entries strictly after this Unix timestamp
$since = isset($_GET['since']) ? (int) $_GET['since'] : null;

// ── Read & filter log ─────────────────────────────────────────────────────────
if (!file_exists(DATA_FILE)) {
    exit(json_encode([]));
}

$lines  = file(DATA_FILE, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES);
$result = [];

foreach ($lines as $line) {
    $entry = json_decode($line, true);
    if (!is_array($entry)) continue;
    if (empty($entry['ts'])) continue;   // ts=0 → NTP not synced yet, skip
    $ts = (int) $entry['ts'];
    if ($since !== null && $ts <= $since) continue;  // skip already-known points
    $temp = $entry['probes'][$probe] ?? null;
    if ($temp === null) continue;
    $result[] = [
        'ts'   => $ts,
        'temp' => (int) $temp,
    ];
}

// When not using 'since', slice to the most recent $limit entries
if ($since === null) {
    $result = array_slice($result, -$limit);
}

// Optional: downsample to at most $samples points using bucket averaging.
// Each bucket averages all temps within it and uses the midpoint timestamp.
$samples = isset($_GET['samples']) ? max(1, min((int) $_GET['samples'], MAX_ENTRIES)) : 0;
if ($samples > 0 && count($result) > $samples) {
    $total   = count($result);
    $buckets = $samples;
    $out     = [];
    for ($b = 0; $b < $buckets; $b++) {
        $start = (int) floor($b       * $total / $buckets);
        $end   = (int) floor(($b + 1) * $total / $buckets);
        if ($end <= $start) continue;
        $slice    = array_slice($result, $start, $end - $start);
        $avgTemp  = array_sum(array_column($slice, 'temp')) / count($slice);
        $midTs    = $slice[(int) floor(count($slice) / 2)]['ts'];
        $out[]    = ['ts' => $midTs, 'temp' => (int) round($avgTemp)];
    }
    $result = $out;
}

exit(json_encode(array_values($result)));
 