<?php
/**
 * PitPirate – VAPID key-pair generator
 *
 * Run once from the command line to produce a fresh key pair:
 *   php generate-keys.php
 *
 * Copy the output values into config.php.
 * The PUBLIC key must also be set in the frontend service-worker / subscribe call.
 *
 * Requires PHP with the OpenSSL extension (standard on most hosts).
 */

if (PHP_SAPI !== 'cli') {
    http_response_code(403);
    exit('This script must be run from the command line.');
}

$key = openssl_pkey_new([
    'curve_name'       => 'prime256v1',
    'private_key_type' => OPENSSL_KEYTYPE_EC,
]);

if (!$key) {
    fwrite(STDERR, "ERROR: openssl_pkey_new() failed. Is the OpenSSL extension enabled?\n");
    exit(1);
}

$details = openssl_pkey_get_details($key);

// Raw key bytes
$d = $details['ec']['d'];  // 32-byte private scalar
$x = $details['ec']['x'];  // 32-byte public key x-coordinate
$y = $details['ec']['y'];  // 32-byte public key y-coordinate

// Uncompressed EC point: 0x04 || x || y  (65 bytes total)
$publicKeyRaw = "\x04" . $x . $y;

function b64url(string $bytes): string {
    return rtrim(strtr(base64_encode($bytes), '+/', '-_'), '=');
}

$pub  = b64url($publicKeyRaw);
$priv = b64url($d);

echo PHP_EOL;
echo "VAPID key pair generated successfully." . PHP_EOL;
echo PHP_EOL;
echo "Copy these values into server/php/config.php:" . PHP_EOL;
echo PHP_EOL;
echo "define('VAPID_PUBLIC_KEY',  '{$pub}');" . PHP_EOL;
echo "define('VAPID_PRIVATE_KEY', '{$priv}');" . PHP_EOL;
echo PHP_EOL;
echo "Public key length  : " . strlen($pub)  . " chars (expected 87)" . PHP_EOL;
echo "Private key length : " . strlen($priv) . " chars (expected 43)" . PHP_EOL;
echo PHP_EOL;
