# Dongle <-> PUFMAN Interface

`[]_SK`: signature with `SK`, can be verified with `PK` (TODO: specify the used signing scheme) (TODO: specify its exact size)
`{}_K`: asymmetric encryption with `K`, can be decrypted with its counterpart. (TODO: specify the used encryption scheme)

## Enroll command

`enroll <cert> <payload>`
- `cert`: `Domain || PK_Server || [Domain || PK_Server]_SK_CA`
  - `Domain`: Domain to which the user wants to login. (Maximum: 64 bytes)
  - `PK_Server`: Public key of the server. (32 bytes)
  - `[Domain || PK_Server]_SK_CA`: signature signed by the CA. 