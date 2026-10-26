# XenonRecomp (vendored)

Upstream: https://github.com/hedge-dev/XenonRecomp at commit
`ddd128bcca99fe8bfbb99bea583c972351fa6ace`, with this project's changes applied
on top (they used to live in `patches/xenonrecomp-pes17.patch`; see
`docs/building.md`, "Local changes to XenonRecomp"). Nothing here is a submodule
any more: the sources of its dependencies are part of this tree.

Dependencies in `thirdparty/` (commits as they were cloned):

| directory | upstream | commit |
|---|---|---|
| `fmt` | https://github.com/fmtlib/fmt.git | `873670ba3f9e7bc77ec2c1c94b04f1f8bef77e9f` |
| `libmspack` | https://github.com/kyz/libmspack | `305907723a4e7ab2018e58040059ffb5e77db837` |
| `simde` | https://github.com/simd-everywhere/simde-no-tests.git | `a532a12ca9bbdc5e6547eb602e6256b71a5377d4` |
| `tiny-AES-c` | https://github.com/kokke/tiny-AES-c.git | `23856752fbd139da0b8ca6e471a13d5bcc99a08d` |
| `tomlplusplus` | https://github.com/marzer/tomlplusplus.git | `c4369ae1d8955cae20c4ab40b9813ef4b60e48be` |
| `xxHash` | https://github.com/Cyan4973/xxHash.git | `2bf8313b934633b2a5b7e8fd239645b85e10c852` |

`build/` is not tracked; `build.sh` creates it.
