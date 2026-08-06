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
