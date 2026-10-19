# qrcodegen (vendored)

QR Code generator library by Project Nayuki, C version:
https://github.com/nayuki/QR-Code-generator (files `c/qrcodegen.c`, `c/qrcodegen.h`).

- **License:** MIT. The full notice is at the top of both files.
- **Version:** commit 3c6d0b3cefb4e049dc337e82237c9644399716a8 (2026-08-31), unmodified.
- **Used by:** `core/overlay.c`, to show the PSP's URL as a QR code.
- **Build:** compiled by the Makefile with `-DNDEBUG -w`. `abs`/`labs`/`memset`/`memmove`/`strlen` come from `common/mini.c`.
