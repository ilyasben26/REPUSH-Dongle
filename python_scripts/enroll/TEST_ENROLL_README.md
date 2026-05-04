
## Generate a CA key pair:

```python
from cryptography.hazmat.primitives.asymmetric import ed25519
import binascii

ca_privkey = ed25519.Ed25519PrivateKey.generate()
ca_privkey_bytes = ca_privkey.private_bytes_raw()  # 32-byte seed

ca_pubkey = ca_privkey.public_key()
ca_pubkey_bytes = ca_pubkey.public_bytes_raw()  # 32 bytes

with open('ca_private.hex', 'w') as f:
    f.write(binascii.hexlify(ca_privkey_bytes).decode())

with open('ca_public.hex', 'w') as f:
    f.write(binascii.hexlify(ca_pubkey_bytes).decode())

print(f"CA Private Key: {binascii.hexlify(ca_privkey_bytes).decode()}")
print(f"CA Public Key:  {binascii.hexlify(ca_pubkey_bytes).decode()}")
```

>>> print(f"CA Private Key: {binascii.hexlify(ca_privkey_bytes).decode()}")
CA Private Key: c445d81a59072157e5a0753641bd997a89a1020a59f2452930023676cb4d1c8a
>>> print(f"CA Public Key:  {binascii.hexlify(ca_pubkey_bytes).decode()}")
CA Public Key:  6082454eab1c7c1e13c8ab61dbf9324dab0123bff6144eea42b7c868c95badc9


## Generate Server Key Pair

```python
from cryptography.hazmat.primitives.asymmetric import ed25519
import binascii

server_privkey = ed25519.Ed25519PrivateKey.generate()
server_privkey_bytes = server_privkey.private_bytes_raw()

server_pubkey = server_privkey.public_key()
server_pubkey_bytes = server_pubkey.public_bytes_raw()

with open('server_public.hex', 'w') as f:
    f.write(binascii.hexlify(server_pubkey_bytes).decode())

with open('server_private.hex', 'w') as f:
    f.write(binascii.hexlify(server_privkey_bytes).decode())

print(f"Server Public Key: {binascii.hexlify(server_pubkey_bytes).decode()}")
print(f"Server Private Key: {binascii.hexlify(server_privkey_bytes).decode()}")
```
>>> print(f"Server Public Key: {binascii.hexlify(server_pubkey_bytes).decode()}")
Server Public Key: 4d8b666fe8bac270910f368e580a8b7abe4fbf47b8af54306b6467a42dc41b79
>>> print(f"Server Private Key: {binascii.hexlify(server_privkey_bytes).decode()}")
Server Private Key: 92e5351e55037d95302dd064b1af989f8262319b1ccd8087fc62aa3561c8b3fc



### Test with valid certificate

```bash
python3 test_enroll.py \
  --ca-private-key ca_private.hex \
  --port /dev/tty.usbmodem11201 \
  enroll \
  --domain "ilyas.com" \
  --server-pubkey server_public.hex
```
### Test with invalid tampered certificate

```bash
python3 test_enroll.py \
  --ca-private-key ca_private.hex \
  --port /dev/tty.usbmodem11201 \
  enroll-bad \
  --domain "ilyas.com" \
  --server-pubkey server_public.hex
```

## Certificate Format

Generated certificates are 128 bytes:
- Bytes 0-31: Domain name (padded with zeros)
- Bytes 32-63: Server public key (32 bytes)
- Bytes 64-127: Ed25519 signature over (domain || server_pubkey)

Encoded as base64 before sending to Arduino.
