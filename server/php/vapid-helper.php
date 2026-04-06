<?php
/**
 * Pure-PHP VAPID helper — no Composer required.
 * Uses only PHP's built-in OpenSSL extension.
 *
 * Implements:
 *   - VAPID JWT (ES256) signing
 *   - Web Push payload encryption (RFC 8291 / draft-ietf-webpush-encryption-08)
 *     ECDH-ES key agreement + HKDF-SHA-256 + AES-128-GCM
 */

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

function b64url_encode(string $data): string {
    return rtrim(strtr(base64_encode($data), '+/', '-_'), '=');
}

function b64url_decode(string $data): string {
    $padded = str_pad(strtr($data, '-_', '+/'), strlen($data) + (4 - strlen($data) % 4) % 4, '=');
    return base64_decode($padded);
}

// ---------------------------------------------------------------------------
// VAPID JWT (ES256)
// ---------------------------------------------------------------------------

/**
 * Build and sign a VAPID JWT token.
 *
 * @param string $endpoint         The push endpoint URL
 * @param string $privateKeyB64url VAPID private key (base64url, 32 bytes)
 * @param string $subject          mailto: or https: URL
 * @return string                  "vapid t=<jwt>,k=<pubkey>"
 */
function build_vapid_header(string $endpoint, string $privateKeyB64url, string $publicKeyB64url, string $subject): string {
    $parts  = parse_url($endpoint);
    $origin = $parts['scheme'] . '://' . $parts['host'];
    if (!empty($parts['port'])) {
        $origin .= ':' . $parts['port'];
    }

    $header  = b64url_encode(json_encode(['typ' => 'JWT', 'alg' => 'ES256']));
    $payload = b64url_encode(json_encode([
        'aud' => $origin,
        'exp' => time() + 43200, // 12 hours
        'sub' => $subject,
    ]));

    $signingInput = $header . '.' . $payload;

    // Reconstruct PEM private key from raw 32-byte scalar
    $dRaw = b64url_decode($privateKeyB64url);
    $privPem = ec_private_key_to_pem($dRaw);

    $signature = '';
    if (!openssl_sign($signingInput, $signature, $privPem, OPENSSL_ALGO_SHA256)) {
        throw new RuntimeException('openssl_sign failed: ' . openssl_error_string());
    }

    // openssl_sign returns DER-encoded ASN.1 signature; convert to raw r||s (64 bytes)
    $signature = der_to_raw_rs($signature);

    $jwt = $signingInput . '.' . b64url_encode($signature);

    return 'vapid t=' . $jwt . ',k=' . $publicKeyB64url;
}

/**
 * Encode a raw 32-byte EC private key scalar into a PEM PKCS#8 / SEC1 key
 * that openssl can use for signing on P-256.
 *
 * We reconstruct the public key point by multiplying G, but the easier
 * approach is to import d into an openssl key via the PKCS#12 DER structure.
 * Simplest portable method: rebuild a minimal SEC1 ECPrivateKey DER and wrap it.
 */
function ec_private_key_to_pem(string $dRaw): string {
    // We need the public key too. Re-derive it using openssl:
    // Create a key pair from the raw d by embedding it in a minimal DER blob.

    // SEC1 ECPrivateKey DER (RFC 5915) for P-256:
    // SEQUENCE {
    //   INTEGER 1
    //   OCTET STRING (d)
    //   [0] OID 1.2.840.10045.3.1.7 (prime256v1)
    // }
    // But we can use a simpler approach: provide d to openssl_pkey_new via
    // a PKCS#8 DER blob. Build it manually.

    // Minimal EC private key DER for P-256 without embedded public key.
    // openssl can derive the public key from d.
    $oid_p256 = "\x06\x08\x2a\x86\x48\xce\x3d\x03\x01\x07"; // OID 1.2.840.10045.3.1.7
    $oid_ec   = "\x06\x07\x2a\x86\x48\xce\x3d\x02\x01";      // OID 1.2.840.10045.2.1

    // SEC1 ECPrivateKey inner:
    // SEQUENCE { INTEGER 1, OCTET STRING d, [0] { OID p256 } }
    $version = "\x02\x01\x01";                         // INTEGER 1
    $dOctet  = "\x04\x20" . $dRaw;                     // OCTET STRING (32 bytes)
    $oidCtx  = "\xa0" . chr(strlen($oid_p256)) . $oid_p256; // [0] OID

    $inner = $version . $dOctet . $oidCtx;
    $sec1  = "\x30" . der_length(strlen($inner)) . $inner;

    // PKCS#8: SEQUENCE { SEQUENCE { OID ec, OID p256 }, OCTET STRING { sec1 } }
    $algId = "\x30" . der_length(strlen($oid_ec) + strlen($oid_p256))
           . $oid_ec . $oid_p256;
    $privOctet = "\x04" . der_length(strlen($sec1)) . $sec1;
    $pkcs8Version = "\x02\x01\x00"; // version = 0
    $pkcs8Inner = $pkcs8Version . $algId . $privOctet;
    $pkcs8 = "\x30" . der_length(strlen($pkcs8Inner)) . $pkcs8Inner;

    $pem = "-----BEGIN PRIVATE KEY-----\n"
         . chunk_split(base64_encode($pkcs8), 64, "\n")
         . "-----END PRIVATE KEY-----\n";

    return $pem;
}

function der_length(int $len): string {
    if ($len < 128) {
        return chr($len);
    }
    if ($len < 256) {
        return "\x81" . chr($len);
    }
    return "\x82" . chr($len >> 8) . chr($len & 0xff);
}

/**
 * Convert a DER-encoded ASN.1 ECDSA signature to raw 64-byte r||s.
 */
function der_to_raw_rs(string $der): string {
    // SEQUENCE { INTEGER r, INTEGER s }
    $offset = 2; // skip SEQUENCE tag + length
    // INTEGER tag
    if (ord($der[$offset]) !== 0x02) throw new RuntimeException('Bad DER sig');
    $offset++;
    $rLen = ord($der[$offset++]);
    $r    = substr($der, $offset, $rLen);
    $offset += $rLen;
    if (ord($der[$offset]) !== 0x02) throw new RuntimeException('Bad DER sig');
    $offset++;
    $sLen = ord($der[$offset++]);
    $s    = substr($der, $offset, $sLen);

    // Strip leading 0x00 padding, then left-pad to 32 bytes
    $r = ltrim($r, "\x00");
    $s = ltrim($s, "\x00");
    $r = str_pad($r, 32, "\x00", STR_PAD_LEFT);
    $s = str_pad($s, 32, "\x00", STR_PAD_LEFT);

    return $r . $s;
}

// ---------------------------------------------------------------------------
// Web Push payload encryption (RFC 8291)
// ---------------------------------------------------------------------------

/**
 * Encrypt a plaintext string for delivery to a push subscription.
 *
 * @param string $plaintext The notification payload (JSON string)
 * @param string $authB64   subscription.keys.auth   (base64url)
 * @param string $p256dhB64 subscription.keys.p256dh (base64url, 65-byte uncompressed point)
 * @return array ['ciphertext' => string, 'salt' => string, 'serverPublicKey' => string]
 *               All binary strings. serverPublicKey is 65-byte uncompressed point.
 */
function encrypt_push_payload(string $plaintext, string $authB64, string $p256dhB64): array {
    $auth    = b64url_decode($authB64);
    $uaPublicKey = b64url_decode($p256dhB64); // 65-byte uncompressed EC point

    // 1. Generate ephemeral server key pair
    $serverKey  = openssl_pkey_new([
        'curve_name'       => 'prime256v1',
        'private_key_type' => OPENSSL_KEYTYPE_EC,
    ]);
    $serverDetails = openssl_pkey_get_details($serverKey);
    $serverPublicKey = "\x04" . $serverDetails['ec']['x'] . $serverDetails['ec']['y']; // 65 bytes

    // 2. Reconstruct UA public key as openssl key object
    $uaKey = public_key_from_uncompressed($uaPublicKey);

    // 3. ECDH shared secret
    $sharedSecret = ecdh($serverKey, $uaKey);

    // 4. Salt (16 random bytes)
    $salt = random_bytes(16);

    // 5. HKDF-SHA256 PRK (using auth as salt, sharedSecret as IKM)
    $prk = hkdf_sha256($auth, $sharedSecret, "Content-Encoding: auth\x00", 32);

    // 6. HKDF derive Content Encryption Key (16 bytes) and nonce (12 bytes)
    $context = build_context($uaPublicKey, $serverPublicKey);
    $cek     = hkdf_sha256($salt, $prk, "Content-Encoding: aesgcm\x00" . $context, 16);
    $nonce   = hkdf_sha256($salt, $prk, "Content-Encoding: nonce\x00"   . $context, 12);

    // 7. Add 2-byte padding length prefix (0) + plaintext, then AES-128-GCM encrypt
    $padded = "\x00\x00" . $plaintext; // no padding
    $tag    = '';
    $ciphertext = openssl_encrypt($padded, 'aes-128-gcm', $cek, OPENSSL_RAW_DATA, $nonce, $tag, '', 16);
    if ($ciphertext === false) {
        throw new RuntimeException('openssl_encrypt failed: ' . openssl_error_string());
    }

    return [
        'ciphertext'      => $ciphertext . $tag,
        'salt'            => $salt,
        'serverPublicKey' => $serverPublicKey,
    ];
}

/**
 * Build P-256 context string: length-prefixed UA public key || server public key
 */
function build_context(string $uaPublicKey, string $serverPublicKey): string {
    $uaLen  = strlen($uaPublicKey);     // 65
    $srvLen = strlen($serverPublicKey); // 65
    return "P-256\x00"
         . chr(0) . chr($uaLen)  . $uaPublicKey
         . chr(0) . chr($srvLen) . $serverPublicKey;
}

/**
 * HKDF-SHA-256 with the given salt, IKM, info string, and output length.
 * RFC 5869 Extract-then-Expand.
 */
function hkdf_sha256(string $salt, string $ikm, string $info, int $length): string {
    // Extract
    $prk = hash_hmac('sha256', $ikm, $salt, true);
    // Expand (single block since length <= 32)
    $t   = '';
    $okm = '';
    for ($i = 1; strlen($okm) < $length; $i++) {
        $t   = hash_hmac('sha256', $t . $info . chr($i), $prk, true);
        $okm .= $t;
    }
    return substr($okm, 0, $length);
}

/**
 * Import a 65-byte uncompressed EC P-256 public key into an openssl key object.
 */
function public_key_from_uncompressed(string $raw): mixed {
    // Wrap in SubjectPublicKeyInfo DER
    $oid_ec   = "\x06\x07\x2a\x86\x48\xce\x3d\x02\x01";
    $oid_p256 = "\x06\x08\x2a\x86\x48\xce\x3d\x03\x01\x07";

    $algId   = "\x30" . der_length(strlen($oid_ec) + strlen($oid_p256)) . $oid_ec . $oid_p256;
    $bitStr  = "\x03" . der_length(strlen($raw) + 1) . "\x00" . $raw;
    $spki    = "\x30" . der_length(strlen($algId) + strlen($bitStr)) . $algId . $bitStr;

    $pem = "-----BEGIN PUBLIC KEY-----\n"
         . chunk_split(base64_encode($spki), 64, "\n")
         . "-----END PUBLIC KEY-----\n";

    $key = openssl_pkey_get_public($pem);
    if (!$key) {
        throw new RuntimeException('Failed to import UA public key: ' . openssl_error_string());
    }
    return $key;
}

/**
 * Perform ECDH key exchange: server private key × UA public key → shared secret bytes.
 */
function ecdh(mixed $serverPrivate, mixed $uaPublic): string {
    // Use openssl_dh_compute_key equivalent for EC via a temporary export trick.
    // PHP 8.1+ has openssl_pkey_derive; for compatibility we use it if available,
    // otherwise fall back to the custom derivation.
    if (function_exists('openssl_pkey_derive')) {
        $secret = openssl_pkey_derive($uaPublic, $serverPrivate, 32);
        if ($secret === false) {
            throw new RuntimeException('openssl_pkey_derive failed: ' . openssl_error_string());
        }
        return $secret;
    }

    throw new RuntimeException(
        'openssl_pkey_derive() is not available. PHP 7.3+ with OpenSSL 1.0.2+ is required.'
    );
}
