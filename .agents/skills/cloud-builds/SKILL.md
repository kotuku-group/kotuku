---
name: cloud-builds
description: Build and install Kōtuku in ephemeral cloud environments. Use when working in a cloud or container session where the build tree may be missing, when builds are slow and unnecessary modules should be disabled, or when origo is not yet installed at build/agents-install.
---

# Building Kōtuku in the Cloud

When working in ephemeral cloud environments:

- Prefer the pre-created build tree at `build/agents` and install tree at `build/agents-install` to avoid the expense
  of repeated configuration.  If the directory exists you can update it with
  `cmake --build build/agents --config Debug --parallel`.
- If `origo` is not already installed at `build/agents-install` then performing the build and install process is
  essential if intending to run `origo` for Tiri scripts and Flute tests.
- If configuring a build, disabling unnecessary modules like Audio and Graphics features (if they are not relevant)
  will speed up compilation.  If *certain* that the environment is cloud-based, you can consider including the
  following with your CMake build configuration:
  `-DDISABLE_AUDIO=ON -DDISABLE_X11=ON -DDISABLE_DISPLAY=ON -DDISABLE_FONT=ON`

## Dependency downloads failing with HTTP 403

In cloud sessions the network proxy may refuse GitHub archive URLs (`https://github.com/<owner>/<repo>/archive/...`),
so `ExternalProject` downloads such as zlib, libjpeg-turbo and libffi fail with `HTTP response code said error`
and a 403 status.  Git cloning from GitHub is permitted, so rebuild the archive locally instead:

1. Clone the pinned tag: `git clone --depth 1 --branch <tag> https://github.com/<owner>/<repo>.git <repo>-<version>`
2. Recreate the archive: `git -C <repo>-<version> archive --format=tar --prefix=<repo>-<version>/ <tag> | gzip -cn > <file>.tar.gz`
3. Check that `sha512sum` matches the `URL_HASH` pinned in CMake.  Never edit `URL_HASH` or the download URL to make
   a mismatched file pass.
4. Copy the file to the path named in the failing `download-*.cmake` script
   (e.g. `build/agents/zlib/src/v1.3.2.tar.gz`) and rebuild.  CMake skips the download when the hash matches.

Do this in a scratch directory outside the repository.  If the host is not GitHub, or the hash does not match,
stop and report the blocked host to the user rather than working around it.
