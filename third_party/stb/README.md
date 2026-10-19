# stb_image_write (vendored)

Sean Barrett's single-file image writer:
https://github.com/nothings/stb (file `stb_image_write.h`).

- **License:** public domain or MIT, your choice. The notice is at the end of the file.
- **Version:** v1.16, as on `master` on 2026-10-05; the file was last changed in commit
  1ee679ca2ef753a528db5ba6801e1067b40481b8 (2021-07-12). Unmodified.
- **Used by:** `user/shot.c`, through `stb_jpeg.c`, to send screenshots as JPEG.
- **Build:** `stb_jpeg.c` (ours) compiles the implementation with everything `static`
  and no stdio, so only the JPEG writer is kept; the PNG/TGA/HDR writers and their
  `malloc`/libm calls are dropped. Compiled by the Makefile with `-DNDEBUG -w`.
