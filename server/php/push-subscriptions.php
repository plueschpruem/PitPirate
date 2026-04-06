<?php
/**
 * PitPirate – browser push-notification subscription manager
 *
 * POST /php/push-subscriptions.php?action=subscribe
 *   Body: the PushSubscription JSON object from the browser
 *         { endpoint, keys: { auth, p256dh } }
 *   Response: { ok: true }
 *
 * POST /php/push-subscriptions.php?action=unsubscribe
 *   Body: { "endpoint": "<pushEndpointUrl>" }
 *   Response: { ok: true }
 *
 * No authentication is required for subscribe/unsubscribe — the push
 * endpoint URL itself is the opaque capability token.
 */

require_once __DIR__ . '/config.php';

// ---------------------------------------------------------------------------
// Debug logger — writes to api_debug.log in the same directory
// ---------------------------------------------------------------------------
define('DEBUG_LOG', __DIR__ . '/api_debug.log');
function dbg(string $msg): void {
    $line = date('Y-m-d H:i:s') . ' | ' . $msg . PHP_EOL;
    file_put_contents(DEBUG_LOG, $line, FILE_APPEND | LOCK_EX);
}

$method = $_SERVER['REQUEST_METHOD'] ?? '?';
$action = $_GET['action'] ?? '(none)';
$origin = $_SERVER['HTTP_ORIGIN'] ?? '-';
$ua     = $_SERVER['HTTP_USER_AGENT'] ?? '-';
dbg(">>> $method ?action=$action  origin=$origin  ua=$ua");

header('Content-Type: application/json');
header('Access-Control-Allow-Origin: *');
header('Access-Control-Allow-Methods: GET, POST, OPTIONS');
header('Access-Control-Allow-Headers: Content-Type');

if ($_SERVER['REQUEST_METHOD'] === 'OPTIONS') {
    dbg('--- preflight 204');
    http_response_code(204);
    exit;
}

// vapid-public-key is a safe read — allow GET
if ($_SERVER['REQUEST_METHOD'] === 'GET') {
    if (($_GET['action'] ?? '') === 'vapid-public-key') {
        dbg('<<< 200 vapid-public-key');
        echo json_encode(['publicKey' => VAPID_PUBLIC_KEY]);
        exit;
    }
    // Debug log viewer — ?action=debug-log  (remove in production!)
    if (($_GET['action'] ?? '') === 'debug-log') {
        header('Content-Type: text/plain; charset=utf-8');
        if (file_exists(DEBUG_LOG)) {
            // Return last 200 lines so it doesn't get huge
            $lines = file(DEBUG_LOG);
            echo implode('', array_slice($lines, -200));
        } else {
            echo "(log is empty)\n";
        }
        exit;
    }
    dbg('<<< 405 GET with unknown action');
    http_response_code(405);
    echo json_encode(['error' => 'Method not allowed']);
    exit;
}

if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
    dbg('<<< 405 unexpected method: ' . $method);
    http_response_code(405);
    echo json_encode(['error' => 'Method not allowed']);
    exit;
}

$action = $_GET['action'] ?? '';

// ---------------------------------------------------------------------------
// Ensure data directory and subscriptions file exist
// ---------------------------------------------------------------------------
$dataDir = dirname(SUBSCRIPTIONS_FILE);
if (!is_dir($dataDir)) {
    mkdir($dataDir, 0750, true);
}
if (!file_exists(SUBSCRIPTIONS_FILE)) {
    file_put_contents(SUBSCRIPTIONS_FILE, json_encode([]));
}

// ---------------------------------------------------------------------------
// action=subscribe
// ---------------------------------------------------------------------------
if ($action === 'subscribe') {
    $body = (string) file_get_contents('php://input');
    $sub  = json_decode($body, true);
    dbg('subscribe body len=' . strlen($body) . ' endpoint=' . ($sub['endpoint'] ?? '(missing)'));

    if (empty($sub['endpoint'])
        || empty($sub['keys']['auth'])
        || empty($sub['keys']['p256dh'])) {
        dbg('<<< 400 invalid subscription object');
        http_response_code(400);
        echo json_encode(['error' => 'Invalid subscription object']);
        exit;
    }

    // Only https: endpoints are valid Web Push endpoints
    $scheme = parse_url($sub['endpoint'], PHP_URL_SCHEME);
    if ($scheme !== 'https') {
        dbg('<<< 400 bad endpoint scheme: ' . $scheme);
        http_response_code(400);
        echo json_encode(['error' => 'Invalid endpoint scheme (must be https)']);
        exit;
    }

    // Upsert: replace existing entry with the same endpoint
    $subs = json_decode(file_get_contents(SUBSCRIPTIONS_FILE), true) ?? [];
    $subs = array_values(array_filter(
        $subs,
        fn($s) => ($s['endpoint'] ?? '') !== $sub['endpoint']
    ));
    $subs[] = [
        'endpoint' => $sub['endpoint'],
        'keys'     => [
            'auth'   => $sub['keys']['auth'],
            'p256dh' => $sub['keys']['p256dh'],
        ],
    ];

    file_put_contents(SUBSCRIPTIONS_FILE, json_encode(array_values($subs)));
    dbg('<<< 200 subscribed total=' . count($subs));
    echo json_encode(['ok' => true, 'subscribers' => count($subs)]);
    exit;
}

// ---------------------------------------------------------------------------
// action=unsubscribe
// ---------------------------------------------------------------------------
if ($action === 'unsubscribe') {
    $body     = (string) file_get_contents('php://input');
    $data     = json_decode($body, true);
    $endpoint = trim($data['endpoint'] ?? '');

    if ($endpoint === '') {
        http_response_code(400);
        echo json_encode(['error' => 'endpoint required']);
        exit;
    }

    $subs = json_decode(file_get_contents(SUBSCRIPTIONS_FILE), true) ?? [];
    $subs = array_values(array_filter(
        $subs,
        fn($s) => ($s['endpoint'] ?? '') !== $endpoint
    ));
    file_put_contents(SUBSCRIPTIONS_FILE, json_encode($subs));
    dbg('<<< 200 unsubscribed remaining=' . count($subs));
    echo json_encode(['ok' => true, 'subscribers' => count($subs)]);
    exit;
}

// ---------------------------------------------------------------------------
// action=vapid-public-key  (browser needs this to subscribe)
// ---------------------------------------------------------------------------
if ($action === 'vapid-public-key') {
    echo json_encode(['publicKey' => VAPID_PUBLIC_KEY]);
    exit;
}

dbg('<<< 400 unknown action: ' . $action);
http_response_code(400);
echo json_encode(['error' => 'Unknown action. Use: subscribe | unsubscribe | vapid-public-key']);
