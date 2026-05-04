#!/usr/bin/env python3
"""
Test script for REPUSH Dongle enrollment certificate verification.

Generates valid and invalid certificates signed by the CA and sends them
to the Arduino via serial for testing the enroll command.

Usage:
    # Generate a valid certificate and print it
    python3 test_enroll.py --ca-private-key ca_private.hex gen-cert --domain "example.com" --server-pubkey server_pubkey.hex

    # Generate and send valid cert to Arduino
    python3 test_enroll.py --ca-private-key ca_private.hex --port /dev/ttyUSB0 enroll --domain "example.com" --server-pubkey server_pubkey.hex

    # Generate and send invalid cert (tampered signature)
    python3 test_enroll.py --ca-private-key ca_private.hex --port /dev/ttyUSB0 enroll-bad --domain "example.com" --server-pubkey server_pubkey.hex

    # Test with raw bytes instead of hex strings
    python3 test_enroll.py --ca-private-key ca_private.hex enroll-raw --domain-bytes domain.bin --server-pubkey-bytes server_pubkey.bin
"""

import argparse
import base64
import binascii
import sys
import time
from pathlib import Path

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


# Arduino parser expects: enroll <cert_b64> <payload_b64>
# Keep a non-empty default payload token to avoid format rejection.
DEFAULT_ENROLL_PAYLOAD_B64 = "AA=="


def load_hex_key(filepath):
    """Load a hex-encoded key from file."""
    with open(filepath, 'r') as f:
        hex_str = f.read().strip()
    return binascii.unhexlify(hex_str)


def load_binary_file(filepath):
    """Load raw binary data from file."""
    with open(filepath, 'rb') as f:
        return f.read()


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
    
    # Convert domain to bytes if needed
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
    
    # Sign with CA's private key
    # The ca_privkey should be 64 bytes: 32-byte seed || 32-byte public key
    # But Ed25519PrivateKey.from_private_bytes() expects just the 32-byte seed
    ca_privkey_seed = ca_privkey[:32]
    ca_key = ed25519.Ed25519PrivateKey.from_private_bytes(ca_privkey_seed)
    signature = ca_key.sign(message)
    
    # Certificate is domain(32) || server_pubkey(32) || signature(64)
    cert = domain_padded + server_pubkey + signature
    
    if len(cert) != 128:
        raise ValueError(f"Certificate should be 128 bytes, got {len(cert)}")
    
    return cert


def cert_to_base64(cert):
    """Encode certificate as base64."""
    return base64.b64encode(cert).decode('ascii')


def tamper_signature(cert):
    """Return a copy of cert with the first signature byte modified."""
    cert_bytes = bytearray(cert)
    # Flip the first byte of the signature (last 64 bytes)
    cert_bytes[64] ^= 0xFF
    return bytes(cert_bytes)


def send_enroll_command(port, cert_b64, payload_b64=DEFAULT_ENROLL_PAYLOAD_B64, timeout=2.0):
    """
    Send enroll command to Arduino.
    
    Args:
        port: serial port path
        cert_b64: base64-encoded certificate
        payload_b64: base64-encoded payload (ignored in phase 1)
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
    parser.add_argument('--timeout', type=float, default=2.0, help='Serial read timeout in seconds')
    
    subparsers = parser.add_subparsers(dest='command', help='Command to run')
    
    # gen-cert: generate and print certificate
    gen_parser = subparsers.add_parser('gen-cert', help='Generate certificate and print base64')
    gen_parser.add_argument('--domain', required=True, help='Domain name')
    gen_parser.add_argument('--server-pubkey', required=True, help='Path to server public key (hex-encoded)')
    
    # enroll: generate valid cert and send to Arduino
    enroll_parser = subparsers.add_parser('enroll', help='Generate valid cert and send via enroll command')
    enroll_parser.add_argument('--domain', required=True, help='Domain name')
    enroll_parser.add_argument('--server-pubkey', required=True, help='Path to server public key (hex-encoded)')
    enroll_parser.add_argument('--payload', default=DEFAULT_ENROLL_PAYLOAD_B64,
                               help=f'Base64 payload (optional, default: {DEFAULT_ENROLL_PAYLOAD_B64})')
    
    # enroll-bad: generate cert with tampered signature and send
    enroll_bad_parser = subparsers.add_parser('enroll-bad', help='Generate invalid cert (tampered signature) and send')
    enroll_bad_parser.add_argument('--domain', required=True, help='Domain name')
    enroll_bad_parser.add_argument('--server-pubkey', required=True, help='Path to server public key (hex-encoded)')
    
    # enroll-raw: generate cert from raw binary files
    enroll_raw_parser = subparsers.add_parser('enroll-raw', help='Generate cert from raw binary files and send')
    enroll_raw_parser.add_argument('--domain-bytes', required=True, help='Path to domain bytes file')
    enroll_raw_parser.add_argument('--server-pubkey-bytes', required=True, help='Path to server pubkey bytes file')
    
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
            
            print(f"[+] Generated valid certificate")
            print(f"[*] Connecting to {args.port}...")
            response = send_enroll_command(args.port, cert_b64, args.payload, args.timeout)
            
            print("[+] Response:")
            print(response)
            
            if "CERT_OK" in response:
                print("[+] ✓ Test PASSED: Certificate verified as valid")
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
            
            print(f"[+] Generated invalid certificate (tampered signature)")
            print(f"[*] Connecting to {args.port}...")
            response = send_enroll_command(args.port, cert_b64, DEFAULT_ENROLL_PAYLOAD_B64, args.timeout)
            
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
            
            print(f"[+] Generated certificate from binary files")
            print(f"[*] Connecting to {args.port}...")
            response = send_enroll_command(args.port, cert_b64, DEFAULT_ENROLL_PAYLOAD_B64, args.timeout)
            
            print("[+] Response:")
            print(response)
            
            if "CERT_OK" in response:
                print("[+] ✓ Test PASSED: Certificate verified as valid")
            else:
                print("[!] ✗ Test result unclear")
    
    except Exception as e:
        print(f"[!] Error: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        sys.exit(1)


if __name__ == '__main__':
    main()
