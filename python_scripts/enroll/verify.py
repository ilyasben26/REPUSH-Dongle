import sys
import binascii
from cryptography.hazmat.primitives.asymmetric import ed25519

def verify_signature(public_key_hex, signature_hex, message):
    try:
        # Convert hex strings to raw bytes
        public_key_bytes = binascii.unhexlify(public_key_hex)
        signature_bytes = binascii.unhexlify(signature_hex)
        
        # Ed25519 signatures are 64 bytes long: R (32 bytes) + S (32 bytes)
        print(f"--- Signature Details ---")
        print(f"Signature Length: {len(signature_bytes)} bytes")
        print(f"R (First 32 bytes): {signature_hex[:64].upper()}")
        print(f"S (Last 32 bytes):  {signature_hex[64:].upper()}")
        print(f"-------------------------")
        
        # Load the raw 32-byte public key
        public_key = ed25519.Ed25519PublicKey.from_public_bytes(public_key_bytes)
        
        # Verify the signature against the message
        public_key.verify(signature_bytes, message.encode('utf-8'))
        
        print("✅ Signature is VALID!")
    except Exception as e:
        print(f"❌ Signature is INVALID or verification failed: {e}")

if __name__ == "__main__":
    if len(sys.argv) < 4:
        print("Usage: python3 verify.py <public_key_hex> <signature_hex> <message>")
        print("Example: python3 verify.py 4EE6... A84C... 09testtest")
        sys.exit(1)

    pub_key_arg = sys.argv[1]
    sig_arg = sys.argv[2]
    msg_arg = " ".join(sys.argv[3:])
    
    verify_signature(pub_key_arg, sig_arg, msg_arg)
