# Aris update HTTPS adapter boundary audit

**Owner slice:** `UPDATE-CHECK-HTTPS`
**Reviewed path:** `src/update/UpdateCheckHttpsTransport.cpp`
**Disposition:** `platform-adapter`

This file is the WinINet implementation of the typed update-check transport.
It owns HTTPS session/request setup, bounded redirects, cancellation-aware
handle lifetime, status inspection, and bounded response reads. Manifest
verification and channel policy remain in the pure update domain.

The register records the exact current native-include and native-symbol
fingerprints only. A changed path, rule, context, token, occurrence, or
fingerprint fails closed and requires a fresh review.
