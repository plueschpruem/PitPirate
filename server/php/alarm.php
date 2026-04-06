<?php
/**
 * PitPirate – ESP32 alarm push-notification endpoint
 *
 * The ESP32 POSTs a JSON datagram to this URL whenever a probe temperature
 * crosses a configured alarm limit:
 *
 *   POST https://yourserver.com/alarm.php
 *   Content-Type: application/json
 *   Body: {
 *     "secret":  "<ALARM_SECRET from config.php>",
 *     "title":   "PitPirate",
 *     "message": "Probe 1 HIGH: 125° (limit 120°)",
 *     "tag":     "pitpirate-hi-Probe 1",   // groups/replaces notifications
 *     "silent":  false
 *   }
 *
 * Subscriptions are managed via push-subscriptions.php?action=subscribe from the browser.
 *
 * Dependencies (same directory)
 * ─────────────────────────────
 *  config.php        – VAPID keys, ALARM_SECRET, SUBSCRIPTIONS_FILE
 *  vapid-helper.php  – pure-PHP VAPID + payload encryption
 */

require_once __DIR__ . '/config.php';
require_once __DIR__ . '/vapid-helper.php';

header('Content-Type: application/json');

if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
    http_response_code(405);
    echo json_encode(['error' => 'Method not allowed']);
    exit;
}

// ---------------------------------------------------------------------------
// Parse and validate body
// ---------------------------------------------------------------------------
$raw  = file_get_contents('php://input');
$data = json_decode($raw, true);

if (!is_array($data)) {
    http_response_code(400);
    echo json_encode(['error' => 'Invalid JSON']);
    exit;
}

// Authenticate (constant-time comparison to prevent timing attacks)
$provided = $data['secret'] ?? '';
if (!hash_equals(ALARM_SECRET, $provided)) {
    http_response_code(403);
    echo json_encode(['error' => 'Forbidden']);
    exit;
}

$message = trim($data['message'] ?? '');
if ($message === '') {
    http_response_code(400);
    echo json_encode(['error' => 'Empty message']);
    exit;
}

$title  = trim($data['title']  ?? 'PitPirate');
$tag    = trim($data['tag']    ?? 'pitpirate');
$silent = !empty($data['silent']);
if ($title === '') $title = 'PitPirate';
if ($tag   === '') $tag   = 'pitpirate';

// ---------------------------------------------------------------------------
// Load subscriptions
// ---------------------------------------------------------------------------
$subs = [];
if (file_exists(SUBSCRIPTIONS_FILE)) {
    $subs = json_decode(file_get_contents(SUBSCRIPTIONS_FILE), true) ?? [];
}

if (empty($subs)) {
    http_response_code(200);
    // Not an error — device just has no subscribers yet
    echo json_encode(['ok' => true, 'sent' => 0, 'note' => 'No subscribers']);
    exit;
}

// ---------------------------------------------------------------------------
// Build push payload
// ---------------------------------------------------------------------------
$payload = json_encode([
    'title'    => $title,
    'body'     => $message,
    'silent'   => $silent,
    'tag'      => $tag,
    'renotify' => true,
]);

// ---------------------------------------------------------------------------
// Send to all subscribers
// ---------------------------------------------------------------------------
$results  = [];
$expiredEndpoints = [];

foreach ($subs as $sub) {
    try {
        $encrypted = encrypt_push_payload(
            $payload,
            $sub['keys']['auth'],
            $sub['keys']['p256dh']
        );

        $vapidHeader = build_vapid_header(
            $sub['endpoint'],
            VAPID_PRIVATE_KEY,
            VAPID_PUBLIC_KEY,
            VAPID_SUBJECT
        );

        $status = send_push($sub['endpoint'], $encrypted, $vapidHeader);

        // 410 Gone / 404 Not Found = subscription expired, remove it
        if ($status === 410 || $status === 404) {
            $expiredEndpoints[] = $sub['endpoint'];
        }

        $results[] = [
            'endpoint' => substr($sub['endpoint'], 0, 50) . '...',
            'status'   => $status,
        ];
    } catch (Throwable $e) {
        $results[] = [
            'endpoint' => substr($sub['endpoint'], 0, 50) . '...',
            'error'    => $e->getMessage(),
        ];
    }
}

// Remove expired subscriptions
if (!empty($expiredEndpoints)) {
    $subs = array_values(array_filter(
        $subs,
        fn($s) => !in_array($s['endpoint'], $expiredEndpoints, true)
    ));
    file_put_contents(SUBSCRIPTIONS_FILE, json_encode($subs));
}

echo json_encode(['ok' => true, 'sent' => count($results), 'results' => $results]);

// ---------------------------------------------------------------------------
// cURL push helper
// ---------------------------------------------------------------------------
function send_push(string $endpoint, array $encrypted, string $vapidHeader): int
{
    $ch = curl_init($endpoint);
    curl_setopt_array($ch, [
        CURLOPT_POST           => true,
        CURLOPT_POSTFIELDS     => $encrypted['ciphertext'],
        CURLOPT_RETURNTRANSFER => true,
        CURLOPT_TIMEOUT        => 15,
        CURLOPT_HTTPHEADER     => [
            'Content-Type: application/octet-stream',
            'Content-Encoding: aesgcm',
            'Encryption: salt='    . b64url_encode($encrypted['salt']),
            'Crypto-Key: dh='      . b64url_encode($encrypted['serverPublicKey']) . ';' . $vapidHeader,
            'Authorization: '      . $vapidHeader,
            'TTL: 86400',
        ],
    ]);
    curl_exec($ch);
    $code = curl_getinfo($ch, CURLINFO_HTTP_CODE);
    curl_close($ch);
    return $code;
}
