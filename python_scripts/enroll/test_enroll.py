#!/usr/bin/env python3
"""
Test script for REPUSH Dongle enrollment certificate verification.

Generates valid and invalid certificates signed by the CA and sends them
to the Arduino via serial for testing the enroll command.

Usage:
    # Generate valid cert only
    python3 test_enroll.py --ca-private-key ca_private.hex gen-cert --domain "example.com" --server-pubkey server_public.hex

    # Generate and send valid cert + valid payload
    python3 test_enroll.py --ca-private-key ca_private.hex --port /dev/ttyUSB0 enroll --domain "example.com" --server-pubkey server_public.hex --server-private-key server_private.hex

    # Generate and send invalid cert (tampered cert signature)
    python3 test_enroll.py --ca-private-key ca_private.hex --port /dev/ttyUSB0 enroll-bad --domain "example.com" --server-pubkey server_public.hex --server-private-key server_private.hex

    # Generate and send payload with tampered payload signature
    python3 test_enroll.py --ca-private-key ca_private.hex --port /dev/ttyUSB0 enroll-payload-bad --domain "example.com" --server-pubkey server_public.hex --server-private-key server_private.hex
"""

import argparse
import base64
import binascii
import secrets
import sys
import time

try:
    import serial
    SERIAL_AVAILABLE = True
except ImportError:
    SERIAL_AVAILABLE = False

try:
    from cryptography.hazmat.primitives.asymmetric import ed25519
    CRYPTO_AVAILABLE = True
except ImportError:
    CRYPTO_AVAILABLE = False


CERT_DOMAIN_BYTES = 32
CERT_PK_SERVER_BYTES = 32
CERT_SIG_BYTES = 64
CERT_TOTAL_BYTES = CERT_DOMAIN_BYTES + CERT_PK_SERVER_BYTES + CERT_SIG_BYTES

PAYLOAD_PREFIX = b"login"
PAYLOAD_CHALLENGE_BYTES = 16
PAYLOAD_NONCE_BYTES = 16
PAYLOAD_MESSAGE_BYTES = len(PAYLOAD_PREFIX) + PAYLOAD_CHALLENGE_BYTES + PAYLOAD_NONCE_BYTES
PAYLOAD_TOTAL_BYTES = PAYLOAD_MESSAGE_BYTES + CERT_SIG_BYTES


def load_hex_key(filepath):
    """Load a hex-encoded key from file."""
    with open(filepath, 'r') as f:
        hex_str = f.read().strip()
    return binascii.unhexlify(hex_str)


def load_binary_file(filepath):
    """Load raw binary data from file."""
    with open(filepath, 'rb') as f:
        return f.read()


def parse_hex_exact(hex_str, expected_len, label):
    """Parse hex string and enforce exact length."""
    try:
        raw = binascii.unhexlify(hex_str)
    except binascii.Error as e:
        raise ValueError(f"Invalid {label} hex: {e}")
    if len(raw) != expected_len:
        raise ValueError(f"{label} must be {expected_len} bytes, got {len(raw)}")
    return raw


def pad_to_length(data, length, pad_byte=0x00):
    """Pad data to a fixed length."""
    if len(data) > length:
        raise ValueError(f"Data is {len(data)} bytes, cannot fit in {length} bytes")
    return data + bytes([pad_byte] * (length - len(data)))


def create_certificate(domain, server_pubkey, ca_privkey):
    """
    Create a certificate: domain(32) || server_pubkey(32) || signature(64)
    Signature is over domain || server_pubkey using CA's private key.
    
    Args:
        domain: str or bytes - domain name (will be padded to 32 bytes)
        server_pubkey: bytes - 32-byte server public key
        ca_privkey: bytes - 64-byte Ed25519 private key (seed || public key)
    
    Returns:
        bytes: 128-byte certificate
    """
    if not CRYPTO_AVAILABLE:
        raise RuntimeError("cryptography library not available. Install with: pip install cryptography")
    
    if isinstance(domain, str):
        domain_bytes = domain.encode('utf-8')
    else:
        domain_bytes = domain
    
    # Pad domain to 32 bytes
    domain_padded = pad_to_length(domain_bytes, 32)
    
    # Ensure server pubkey is 32 bytes
    if len(server_pubkey) != 32:
        raise ValueError(f"server_pubkey must be 32 bytes, got {len(server_pubkey)}")
    
    # Message to sign is domain || server_pubkey
    message = domain_padded + server_pubkey
    
    ca_privkey_seed = ca_privkey[:32]
    ca_key = ed25519.Ed25519PrivateKey.from_private_bytes(ca_privkey_seed)
    signature = ca_key.sign(message)
    
    # Certificate is domain(32) || server_pubkey(32) || signature(64)
    cert = domain_padded + server_pubkey + signature
    
    if len(cert) != CERT_TOTAL_BYTES:
        raise ValueError(f"Certificate should be 128 bytes, got {len(cert)}")
    
    return cert


def create_signed_payload(server_privkey, challenge=None, nonce=None):
    """
    Create payload bytes:
    b"login" || challenge(16) || nonce(16) || sig(64)
    where sig = Ed25519_sign(server_sk, b"login"||challenge||nonce)
    """
    if not CRYPTO_AVAILABLE:
        raise RuntimeError("cryptography library not available. Install with: pip install cryptography")

    if challenge is None:
        challenge = secrets.token_bytes(PAYLOAD_CHALLENGE_BYTES)
    if nonce is None:
        nonce = secrets.token_bytes(PAYLOAD_NONCE_BYTES)

    if len(challenge) != PAYLOAD_CHALLENGE_BYTES:
        raise ValueError(f"challenge must be {PAYLOAD_CHALLENGE_BYTES} bytes")
    if len(nonce) != PAYLOAD_NONCE_BYTES:
        raise ValueError(f"nonce must be {PAYLOAD_NONCE_BYTES} bytes")

    message = PAYLOAD_PREFIX + challenge + nonce
    sk = ed25519.Ed25519PrivateKey.from_private_bytes(server_privkey[:32])
    signature = sk.sign(message)
    payload = message + signature

    if len(payload) != PAYLOAD_TOTAL_BYTES:
        raise ValueError(f"payload must be {PAYLOAD_TOTAL_BYTES} bytes, got {len(payload)}")

    return payload, challenge, nonce


def payload_to_base64(payload):
    return base64.b64encode(payload).decode('ascii')


def cert_to_base64(cert):
    """Encode certificate as base64."""
    return base64.b64encode(cert).decode('ascii')


def tamper_signature(cert):
    """Return a copy of cert with the first signature byte modified."""
    cert_bytes = bytearray(cert)
    # Flip the first byte of the signature (last 64 bytes)
    cert_bytes[64] ^= 0xFF
    return bytes(cert_bytes)


def send_enroll_command(port, cert_b64, payload_b64, timeout=2.0):
    """
    Send enroll command to Arduino.
    
    Args:
        port: serial port path
        cert_b64: base64-encoded certificate
        payload_b64: base64-encoded payload
        timeout: read timeout in seconds
    
    Returns:
        str: response from Arduino
    """
    if not SERIAL_AVAILABLE:
        raise RuntimeError("pyserial library not available. Install with: pip install pyserial")
    
    try:
        ser = serial.Serial(port, 115200, timeout=timeout)
        time.sleep(1)  # Wait for Arduino to be ready
        
        # Send enroll command
        command = f"enroll {cert_b64} {payload_b64}\n"
        print(f"[*] Sending: {command.strip()}")
        ser.write(command.encode())
        ser.flush()
        
        # Read response
        response = ""
        start = time.time()
        while time.time() - start < timeout:
            if ser.in_waiting:
                response += ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
        
        ser.close()
        return response
    
    except Exception as e:
        print(f"[!] Error: {e}", file=sys.stderr)
        raise


def main():
    parser = argparse.ArgumentParser(
        description="Test REPUSH Dongle enrollment certificate verification",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )
    
    parser.add_argument('--ca-private-key', required=True, help='Path to CA private key (hex-encoded)')
    parser.add_argument('--port', help='Serial port (e.g., /dev/ttyUSB0). If not provided, only generate cert.')
    parser.add_argument('--timeout', type=float, default=5.0, help='Serial read timeout in seconds')
    
    subparsers = parser.add_subparsers(dest='command', help='Command to run')
    
    # gen-cert: generate and print certificate
    gen_parser = subparsers.add_parser('gen-cert', help='Generate certificate and print base64')
    gen_parser.add_argument('--domain', required=True, help='Domain name')
    gen_parser.add_argument('--server-pubkey', required=True, help='Path to server public key (hex-encoded)')
    
    # enroll: generate valid cert and send to Arduino
    enroll_parser = subparsers.add_parser('enroll', help='Generate valid cert and send via enroll command')
    enroll_parser.add_argument('--domain', required=True, help='Domain name')
    enroll_parser.add_argument('--server-pubkey', required=True, help='Path to server public key (hex-encoded)')
    enroll_parser.add_argument('--server-private-key', required=True, help='Path to server private key (hex-encoded)')
    enroll_parser.add_argument('--challenge-hex', help='Optional 16-byte challenge as 32 hex chars')
    enroll_parser.add_argument('--nonce-hex', help='Optional 16-byte nonce as 32 hex chars')
    
    # enroll-bad: generate cert with tampered signature and send
    enroll_bad_parser = subparsers.add_parser('enroll-bad', help='Generate invalid cert (tampered signature) and send')
    enroll_bad_parser.add_argument('--domain', required=True, help='Domain name')
    enroll_bad_parser.add_argument('--server-pubkey', required=True, help='Path to server public key (hex-encoded)')
    enroll_bad_parser.add_argument('--server-private-key', required=True, help='Path to server private key (hex-encoded)')
    enroll_bad_parser.add_argument('--challenge-hex', help='Optional 16-byte challenge as 32 hex chars')
    enroll_bad_parser.add_argument('--nonce-hex', help='Optional 16-byte nonce as 32 hex chars')

    # enroll-payload-bad: valid cert but tampered payload signature
    enroll_payload_bad_parser = subparsers.add_parser('enroll-payload-bad', help='Generate valid cert with invalid payload signature and send')
    enroll_payload_bad_parser.add_argument('--domain', required=True, help='Domain name')
    enroll_payload_bad_parser.add_argument('--server-pubkey', required=True, help='Path to server public key (hex-encoded)')
    enroll_payload_bad_parser.add_argument('--server-private-key', required=True, help='Path to server private key (hex-encoded)')
    enroll_payload_bad_parser.add_argument('--challenge-hex', help='Optional 16-byte challenge as 32 hex chars')
    enroll_payload_bad_parser.add_argument('--nonce-hex', help='Optional 16-byte nonce as 32 hex chars')
    
    # enroll-raw: generate cert from raw binary files
    enroll_raw_parser = subparsers.add_parser('enroll-raw', help='Generate cert from raw binary files and send')
    enroll_raw_parser.add_argument('--domain-bytes', required=True, help='Path to domain bytes file')
    enroll_raw_parser.add_argument('--server-pubkey-bytes', required=True, help='Path to server pubkey bytes file')
    enroll_raw_parser.add_argument('--server-private-key', required=True, help='Path to server private key (hex-encoded)')
    enroll_raw_parser.add_argument('--challenge-hex', help='Optional 16-byte challenge as 32 hex chars')
    enroll_raw_parser.add_argument('--nonce-hex', help='Optional 16-byte nonce as 32 hex chars')
    
    args = parser.parse_args()
    
    if not args.command:
        parser.print_help()
        sys.exit(1)
    
    # Load CA private key
    print(f"[*] Loading CA private key from {args.ca_private_key}")
    try:
        ca_privkey = load_hex_key(args.ca_private_key)
        if len(ca_privkey) not in [32, 64]:
            print(f"[!] Warning: CA private key is {len(ca_privkey)} bytes. Expected 32 or 64.", file=sys.stderr)
    except Exception as e:
        print(f"[!] Error loading CA private key: {e}", file=sys.stderr)
        sys.exit(1)
    
    try:
        if args.command == 'gen-cert':
            # Load server public key
            server_pubkey = load_hex_key(args.server_pubkey)
            
            # Create certificate
            cert = create_certificate(args.domain, server_pubkey, ca_privkey)
            cert_b64 = cert_to_base64(cert)
            
            print(f"[+] Certificate (base64):")
            print(cert_b64)
            print(f"[+] Length: {len(cert_b64)} chars ({len(base64.b64decode(cert_b64))} bytes)")
        
        elif args.command == 'enroll':
            if not args.port:
                print("[!] --port required for enroll command", file=sys.stderr)
                sys.exit(1)
            
            # Load server public key
            server_pubkey = load_hex_key(args.server_pubkey)
            
            # Create certificate
            cert = create_certificate(args.domain, server_pubkey, ca_privkey)
            cert_b64 = cert_to_base64(cert)

            server_privkey = load_hex_key(args.server_private_key)
            challenge = parse_hex_exact(args.challenge_hex, PAYLOAD_CHALLENGE_BYTES, "challenge") if args.challenge_hex else None
            nonce = parse_hex_exact(args.nonce_hex, PAYLOAD_NONCE_BYTES, "nonce") if args.nonce_hex else None
            payload, challenge, nonce = create_signed_payload(server_privkey, challenge, nonce)
            payload_b64 = payload_to_base64(payload)
            
            print(f"[+] Generated valid certificate")
            print(f"[+] Challenge: {challenge.hex()}")
            print(f"[+] Nonce: {nonce.hex()}")
            print(f"[*] Connecting to {args.port}...")
            response = send_enroll_command(args.port, cert_b64, payload_b64, args.timeout)
            
            print("[+] Response:")
            print(response)
            
            if "CERT_OK" in response and "PAYLOAD_OK" in response:
                print("[+] ✓ Test PASSED: Certificate and payload verified as valid")
            elif "CERT_OK" in response and "PAYLOAD_BAD" in response:
                print("[!] ✗ Test FAILED: Cert valid but payload rejected")
                sys.exit(1)
            elif "CERT_BAD" in response:
                print("[!] ✗ Test FAILED: Certificate should be valid but was rejected")
                sys.exit(1)
            else:
                print("[!] ✗ Unexpected response")
                sys.exit(1)
        
        elif args.command == 'enroll-bad':
            if not args.port:
                print("[!] --port required for enroll-bad command", file=sys.stderr)
                sys.exit(1)
            
            # Load server public key
            server_pubkey = load_hex_key(args.server_pubkey)
            
            # Create certificate
            cert = create_certificate(args.domain, server_pubkey, ca_privkey)
            
            # Tamper with signature
            cert_bad = tamper_signature(cert)
            cert_b64 = cert_to_base64(cert_bad)

            server_privkey = load_hex_key(args.server_private_key)
            challenge = parse_hex_exact(args.challenge_hex, PAYLOAD_CHALLENGE_BYTES, "challenge") if args.challenge_hex else None
            nonce = parse_hex_exact(args.nonce_hex, PAYLOAD_NONCE_BYTES, "nonce") if args.nonce_hex else None
            payload, challenge, nonce = create_signed_payload(server_privkey, challenge, nonce)
            payload_b64 = payload_to_base64(payload)
            
            print(f"[+] Generated invalid certificate (tampered signature)")
            print(f"[+] Challenge: {challenge.hex()}")
            print(f"[+] Nonce: {nonce.hex()}")
            print(f"[*] Connecting to {args.port}...")
            response = send_enroll_command(args.port, cert_b64, payload_b64, args.timeout)
            
            print("[+] Response:")
            print(response)
            
            if "CERT_BAD" in response:
                print("[+] ✓ Test PASSED: Invalid certificate correctly rejected")
            elif "CERT_OK" in response:
                print("[!] ✗ Test FAILED: Invalid certificate should be rejected but was accepted")
                sys.exit(1)
            else:
                print("[!] ✗ Unexpected response")
                sys.exit(1)

        elif args.command == 'enroll-payload-bad':
            if not args.port:
                print("[!] --port required for enroll-payload-bad command", file=sys.stderr)
                sys.exit(1)

            server_pubkey = load_hex_key(args.server_pubkey)
            cert = create_certificate(args.domain, server_pubkey, ca_privkey)
            cert_b64 = cert_to_base64(cert)

            server_privkey = load_hex_key(args.server_private_key)
            challenge = parse_hex_exact(args.challenge_hex, PAYLOAD_CHALLENGE_BYTES, "challenge") if args.challenge_hex else None
            nonce = parse_hex_exact(args.nonce_hex, PAYLOAD_NONCE_BYTES, "nonce") if args.nonce_hex else None
            payload, challenge, nonce = create_signed_payload(server_privkey, challenge, nonce)
            payload_bad = bytearray(payload)
            payload_bad[PAYLOAD_MESSAGE_BYTES] ^= 0x01
            payload_b64 = payload_to_base64(bytes(payload_bad))

            print(f"[+] Generated valid certificate and invalid payload signature")
            print(f"[+] Challenge: {challenge.hex()}")
            print(f"[+] Nonce: {nonce.hex()}")
            print(f"[*] Connecting to {args.port}...")
            response = send_enroll_command(args.port, cert_b64, payload_b64, args.timeout)

            print("[+] Response:")
            print(response)

            if "CERT_OK" in response and "PAYLOAD_BAD" in response:
                print("[+] ✓ Test PASSED: Payload signature correctly rejected")
            elif "CERT_BAD" in response:
                print("[!] ✗ Test FAILED: Certificate unexpectedly rejected")
                sys.exit(1)
            elif "PAYLOAD_OK" in response:
                print("[!] ✗ Test FAILED: Tampered payload unexpectedly accepted")
                sys.exit(1)
            else:
                print("[!] ✗ Unexpected response")
                sys.exit(1)
        
        elif args.command == 'enroll-raw':
            if not args.port:
                print("[!] --port required for enroll-raw command", file=sys.stderr)
                sys.exit(1)
            
            # Load domain and server public key from binary files
            domain_bytes = load_binary_file(args.domain_bytes)
            server_pubkey = load_binary_file(args.server_pubkey_bytes)
            
            # Create certificate
            cert = create_certificate(domain_bytes, server_pubkey, ca_privkey)
            cert_b64 = cert_to_base64(cert)

            server_privkey = load_hex_key(args.server_private_key)
            challenge = parse_hex_exact(args.challenge_hex, PAYLOAD_CHALLENGE_BYTES, "challenge") if args.challenge_hex else None
            nonce = parse_hex_exact(args.nonce_hex, PAYLOAD_NONCE_BYTES, "nonce") if args.nonce_hex else None
            payload, challenge, nonce = create_signed_payload(server_privkey, challenge, nonce)
            payload_b64 = payload_to_base64(payload)
            
            print(f"[+] Generated certificate from binary files")
            print(f"[+] Challenge: {challenge.hex()}")
            print(f"[+] Nonce: {nonce.hex()}")
            print(f"[*] Connecting to {args.port}...")
            response = send_enroll_command(args.port, cert_b64, payload_b64, args.timeout)
            
            print("[+] Response:")
            print(response)
            
            if "CERT_OK" in response and "PAYLOAD_OK" in response:
                print("[+] ✓ Test PASSED: Certificate and payload verified as valid")
            else:
                print("[!] ✗ Test result unclear")
    
    except Exception as e:
        print(f"[!] Error: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        sys.exit(1)


if __name__ == '__main__':
    main()
