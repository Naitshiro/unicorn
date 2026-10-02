# AVX/AVX2 TCG Backport Plan

Branch: `avx-tcg-backport`

## Why

This fork's x86 TCG core is frozen at QEMU 5.0.1, which cannot decode or execute
AVX (VEX-prefixed) instructions. Confirmed empirically: a standalone Unicorn
program executing `vinsertf128 $0x1,%xmm0,%ymm0,%ymm0` (bytes `c4 e3 7d 18 c0 01`)
fails with `UC_ERR_INSN_INVALID` regardless of `uc_ctl` CPU model selection —
CPU-model selection only changes what CPUID *reports*, not what the TCG decoder
can actually execute.

This blocks WindowsEngine (a separate project embedding this fork) from running
real Windows DLLs (msvcrt.dll's memset/memcpy/strlen routines use AVX) past CRT
startup. Disabling AVX in the CPUID advertisement works around the crash but is
a weaker anti-analysis fingerprint (malware sandboxes are commonly detected by
absent AVX in 2026) and isn't a substitute for actually supporting it.

## License note (settled, don't relitigate)

Unicorn is GPLv2, no AGPL/SaaS clause. Running it privately imposes no
obligation; shipping closed-source binaries linking it would require the
combined work be GPLv2. Upstream QEMU and Bochs carry equivalent (GPLv2/LGPL)
terms, so this isn't a reason to avoid Unicorn specifically.

## Why port, not rewrite or switch engines

- Writing a new x86-64 CPU emulator with AVX to comparable fidelity: estimated
  2-3 engineer-years baseline+SSE, +6-12 months for AVX/AVX2, calibrated
  against Bochs's and QEMU's own multi-year AVX development history. Not
  recommended — 1-2 orders of magnitude more expensive than the alternatives.
- Alternatives surveyed (Bochs, upstream QEMU as a library, Triton/Pin/
  DynamoRIO, icicle-emu): none is a clean drop-in replacement for Unicorn's
  hook/API surface without significant porting cost of its own, and most have
  worse AVX fidelity or licensing/embeddability tradeoffs. Bochs (LGPL, real
  AVX/AVX2/AVX-512) is the one serious fallback if this backport stalls.
- Upstream QEMU added a full AVX/AVX2 TCG decoder in v7.2.0 (Paolo Bonzini's
  rewrite: `target/i386/tcg/decode-new.c.inc`, `decode-new.h`, `emit.c.inc`).
  Unicorn's own maintainer, on GitHub issue #1879 ("x86 avx does not work"),
  confirmed AVX landed in QEMU TCG at v7.2.0 while Unicorn is stuck at 5.0.1,
  and suggested porting just the AVX decoder back (not bumping the whole QEMU
  base) is the tractable path.

## Scope decision: skip the unrelated refactor

The commit series between the fork point and v7.2.0 breaks into three groups:

- **(A) Helper prep** (~17 commits): `ops_sse.h` rewritten to 3-operand
  helpers with MMX/XMM/YMM width parameterization; 256-bit helpers added.
- **(B) An unrelated `translate.c` refactor** (~35 commits): `pc_start`
  removal, `DISAS_EOB*`, `gen_jmp_rel`, `TARGET_TB_PCREL`, `cpu_eip`. This
  would collide head-on with Unicorn's own `pc_start`-based hook injection.
- **(C) The new decoder itself**, plus F16C and FMA as follow-on commits.

**Decision: port A and C, skip B entirely.** The new decoder files only use
primitives that already exist in the 5.0.1 base (`gen_lea_modrm_0/1`,
`gen_op_ld_v`/`st_v`, `gen_exception`, etc.) plus a handful of small additions
(constant-temp helpers, YMM state, a few `DisasContext` fields) — none of
which require B's refactor.

Revised estimate with B excluded: **3-6 engineer-weeks**, down from an
earlier rough top-down guess of 1-3 months made without a real diff.

## Unicorn's own divergence from vanilla QEMU (must be preserved)

`translate.c` differs from vanilla QEMU 5.0.1 by ~4,300 lines, but it's
almost entirely mechanical: every TCG call takes a `TCGContext *tcg_ctx`
first argument, `cpu_env` becomes `tcg_ctx->cpu_env`, globals become
`tcg_ctx->...` (1,386 occurrences). The real semantic patches are isolated:

1. `DisasContext` gains `uc` and `prev_pc`.
2. The `disas_insn` prologue: exit-address HLT imitation, `UC_HOOK_CODE` via
   `gen_uc_tracecode`, `sync_eflags`, `check_exit_request`.
3. The `disas_insn` epilogue: patches the `0xf1f1f1f1` instruction-size
   placeholder.
4. `UC_HOOK_TCG_OPCODE` injection in the arithmetic generator.
5. `i386_tr_init_disas_context` / `i386_tr_insn_start` (`prev_pc`).
6. `tcg_x86_init(struct uc_struct *uc)`.

Memory hooks live in the softmmu load/store path, so any newly-ported code
emitting `qemu_ld`/`qemu_st` inherits them automatically. Code hooks and the
instruction-size epilogue are only at risk if a new decoder path returns
early and skips the existing epilogue — every phase that touches
`disas_insn`/`i386_tr_translate_insn` must route through it, not around it.

## Phases

Each phase's regression gate: x86-only Debug build
(`cmake -B build -DUNICORN_ARCH=x86 -DCMAKE_BUILD_TYPE=Debug`), plus a full
multi-arch build (`cmake -B build-all -DCMAKE_BUILD_TYPE=Debug`, no
`-DUNICORN_ARCH`) to catch symbol collisions from any `symbols.sh` changes,
then `ctest` on both.

### Phase 0: TCG and CPU-state infrastructure

Everything the decoder needs from outside `translate.c`'s instruction body.
Split into two parts because flipping the CPUID/XCR0 feature masks before the
decoder exists would make AVX-detecting guest code switch from a working SSE
path to a crashing one — a regression for every current user.

**0a — DONE, uncommitted on `avx-tcg-backport`** (implemented and verified
this session):
- `cpu.h`: `HF_AVX_EN` hflag; explicit `MMXReg`/`XMMReg`/`YMMReg`/`ZMMReg`
  unions (replacing `MMREG_UNION`) with `ZMM_H/X/Y`, `XMM_Q`, `YMM_Q/X`
  accessor macros.
- `helper.c`: `cpu_sync_avx_hflag()`, wired into `cpu_x86_update_cr4`.
- `fpu_helper.c`: XSAVE/XRSTOR support for the YMM upper halves
  (`do_xsave_ymmh`/`do_xrstor_ymmh`/`do_clear_sse`/`do_clear_ymmh`); fixes a
  latent bug where XRSTOR's SSE-clear path `memset`'d the entire `xmm_regs`
  array, which is wrong once YMM state exists alongside it.
- `tcg.h`/`tcg.c`/`tcg-op.h`: `tcg_constant_i32`/`i64`/`vec_matching` shim
  (ordinary temps tracked per-`TCGContext` in a bounded list, released after
  each guest instruction in `i386_tr_translate_insn` — not a real
  `TEMP_CONST` backport, which would touch the register allocator for every
  architecture) plus `tcg_constant_ptr`/`tcg_constant_tl` macros.
- `tcg-op-gvec.h`/`.c`: `tcg_gen_gvec_dup_imm`.
- `symbols.sh` + regenerated `qemu/*.h`: new symbol exports (+5 lines per
  arch header, +6 for `x86_64.h`).
- `tests/unit/test_x86.c`: `test_x86_xrstor_sse_keeps_ymmh`, covering the
  XRSTOR fix.

Verified: x86-only and full multi-arch Debug builds both clean, all 12 test
suites pass including the new test. Does **not** make any AVX instruction
executable yet — that's expected, this phase is infrastructure only.

**0b — deferred to merge alongside Phase 3a:**
- `cpu.c`: add `CPUID_EXT_AVX` to `TCG_EXT_FEATURES`, `CPUID_7_0_EBX_AVX2` to
  `TCG_7_0_EBX_FEATURES` (FMA/F16C wait for Phase 5).
- New tests: `test_x86_avx_cpuid_xcr0` (CPUID.1:ECX.AVX, CPUID.7:EBX.AVX2,
  XCR0 via XGETBV), `test_x86_xsave_xrstor_ymmh` (full round-trip).
- **Caveat surfaced during planning, needs a quick look before this merges:**
  WindowsEngine installs its own CPUID instruction hook
  (`WindowsEngine/src/System/DefaultHookRegistrations.cpp`,
  `InstructionHooks::Cpuid`) that may already report host-like AVX-present
  CPUID values independent of Unicorn's internal masks. XGETBV is not
  similarly hooked there, so guest code checking XCR0 sees Unicorn's real
  value (3 before 0b, 7 after). Whether real msvcrt takes the AVX path today
  depends on which of the two checks it actually uses — worth confirming
  before relying on 0b's mask flip as the sole gate.

### Phase 1: Helper rewrite (upstream group A)

Replace `ops_sse.h`/`ops_sse_header.h` with the 7.2 versions (2,562 and 415
lines), move `gen_sse` call sites onto the new 3-operand helper signatures.
Apply upstream commits in order (see hand-off notes below for the exact list
Opus identified), running the `tcg_ctx` mechanical rewrite after each.

Files: `qemu/target/i386/{ops_sse.h,ops_sse_header.h,helper.h,fpu_helper.c,
int_helper.c,translate.c}` (the `gen_sse` region), `symbols.sh`, regenerated
`qemu/x86_64.h`.

Highest-risk phase — must land before 3, 4, and 5. Done when: no behavior
change, `test_x86` passes, and a new SSE2/SSSE3/SSE4.1 golden test (paddb,
pshufb, pcmpeqb, pmovmskb, movdqu, dpps, blendvps) passes identically before
and after.

### Phase 2: Decoder core

Add `decode-new.h`, `decode-new.c.inc`, `emit.c.inc` skeleton. Glue into
`translate.c`: add `insn_get_signed`/`insn_get_addr`, `has_modrm`, `vex_w`,
`cpuid_7_0_ecx_features` to `DisasContext`; adapt `gen_exception` calls;
dispatch `0xc4`/`0xc5` (VEX prefixes) only when safe (64-bit mode, or
modrm.mod==3 in 32-bit mode, avoiding collision with LES/LDS).

Critical rule: the new decoder path must return through the existing
Unicorn epilogue (the `0xf1f1f1f1` instruction-size patch), not around it, or
`UC_HOOK_CODE` reports a bogus instruction size.

Done when: build passes, `test_x86` passes, and the unsupported-VEX path
still cleanly raises `UC_ERR_INSN_INVALID` (i.e. the skeleton doesn't yet
claim to handle instructions it can't actually emit code for).

### Phase 3: 128-bit and 256-bit AVX/AVX2 coverage (0F opcode map)

Split for incremental delivery:
- **3a**: mov/load/store (`vmovdqu`, `vmovaps`/`ups`) plus `vpcmpeqb`,
  `vpmovmskb`, `vpxor`, `vzeroupper` — the specific subset real msvcrt's
  `memset`/`memcpy`/`strlen` actually use. **Merge Phase 0b here.**
- **3b**: the rest of the `0F` map — remaining integer ALU, FP ops,
  shuffles/compares, `VLDMXCSR`/`VSTMXCSR`.

Done when: a VEX golden test compares YMM results (`uc_reg_read` on
`UC_X86_REG_YMMn`) against native AVX2 hardware output, and a real msvcrt
`memset`/`memcpy` run under WindowsEngine gets past CRT startup.

### Phase 4: 0F38/0F3A maps, remove the old decoder

`vinsertf128`, `vperm2f128`, `vpblendd`, `vpalignr`, `vpbroadcast*`,
`vpermd`, `vpshufb`, `vpmaskmov`, gathers. Then delete `gen_sse` and the old
`sse_op_table*` dispatch tables entirely, and remove the temporary helper
renames from Phase 1.

Done when: the original repro (`c4 e3 7d 18 c0 01`, `vinsertf128`) returns
`UC_ERR_OK` with correct YMM0, the Phase 1 SSE golden test still passes, and
full `ctest` is green.

### Phase 5: FMA and F16C (optional)

`vfmadd231ps`, `vcvtph2ps`, plus their CPUID mask bits. Done when both have
passing golden tests.

### Phase 6: Hardening

- XSAVE/XRSTOR round-trips the YMM upper halves correctly.
- `vzeroupper` works correctly across a `UC_HOOK_CODE` callback boundary.
- Memory-hook addresses fire correctly for a 32-byte `vmovdqu`.
- Unaligned `vmovdqa`/`vmovaps` raise `#GP` as real hardware does.
- Re-run the WindowsEngine real-DLL test end to end, reading the full trace
  line by line rather than grepping for known-error patterns.

## Hand-off notes for implementers

- Phases 0 and 2 can proceed in parallel. Phase 1 must land before 3, 4, 5.
- Every phase needs: the `tcg_ctx` mechanical-rewrite approach (add
  `tcg_ctx,` as first arg, `cpu_env` -> `tcg_ctx->cpu_env`, globals get the
  `tcg_ctx->` prefix), the exact upstream commit hash list for that phase
  (recorded in the planning session's transcript — re-derive via `git log`
  against a real upstream QEMU checkout at the fork point and v7.2.0 if the
  hash list isn't available), and the rule that any new `disas_insn*` path
  returns through the existing Unicorn epilogue.
- Regression gate for every phase: x86-only Debug build + full multi-arch
  Debug build + `ctest` on both, matching what Phase 0a's implementation
  already established as the working verification loop.
- No commits happen automatically — the user reviews and commits each phase
  themselves.


## Phase 0b + 3a spec (dry-run verified, 2026-09-29)

One commit. Verified end-to-end on fresh clones of `avx-tcg-backport` @ `1e5d15c1`: the driver
below reproduces the tested tree byte-for-byte, x86-only Debug build has zero new warnings,
`test_x86` 66/66 pass, and the full multi-arch Debug build + `ctest` passes 12/12.

### Premise corrections found during the dry run (read before implementing)

1. **msvcrt uses VEX.256, not VEX.128.** A disassembly of the real x64 `msvcrt.dll`
   (md5 `a6909fd7bca299386d35aeafae36a207`, from `torito-vfs-root`) shows the VEX set:
   `vmovaps` ymm stores (28), `vmovups` ymm stores (24), `vzeroupper` (11), `vinsertf128`
   (8; this is the 0F3A map, planned for Phase 4), `vpxor` xmm (3), `vpmovmskb` ymm (3),
   `vpcmpeqw` ymm (2), `vpcmpeqb` ymm (1). x64 `ntdll.dll` uses the same set, and
   `vcruntime140.dll` adds `vmovdqu`/`vmovntdq`/`vmovdqa` ymm. 32-bit SysWOW64 msvcrt uses no
   VEX at all. So "VEX.L=1 stays rejected" would leave msvcrt broken; this phase enables L=1.
2. **Unicorn's CPUID is not what gates msvcrt.** msvcrt contains no `cpuid`/`xgetbv`. It picks
   the AVX path via `IsProcessorFeaturePresent(0x27 PF_AVX / 0x28 PF_AVX2)`, and WindowsEngine's
   hook (`WindowsEngine/src/Hooks/DllHooks/Kernel32.cpp`, `IsProcessorFeaturePresent`) answers
   from host CPUID. On an AVX2 host, msvcrt is already on the YMM path today (currently
   `UC_ERR_INSN_INVALID`). Phase 0b still matters: the new decoder itself checks Unicorn's
   CPUID bits (`cpuid(AVX)`/`cpuid(AVX2)` in the tables) and `HF_AVX_EN` (CR4.OSXSAVE plus XCR0
   YMM), so without 0b every VEX vector op stays `#UD`.
3. **Porting "just the msvcrt subset" is not the cheapest safe path.** Upstream's 13-commit
   series `92ec056a6b..f8d19eec0d` applies with zero fuzz on top of Phase 1/2, and 4 of 5 files
   land byte-identical to upstream `f8d19eec0d`. Cherry-picking entries was tried first, and the
   later hunks conflict because emit functions are alphabetized. So this phase ports the whole
   series: the full VEX 0F, 0F38 and 0F3A maps, including `vinsertf128`, most of Phase 3b and
   the decoder half of Phase 4. Excluded: VLDMXCSR/VSTMXCSR (`57f6bba023`), 3DNow, F16C, FMA,
   and removal of the old decoder. Legacy (non-VEX) SSE still goes through the old `gen_sse`:
   only `0xC4/0xC5` reach `disas_insn_new`, as in Phase 2.

### What the change contains

| Piece | Source |
|---|---|
| 0F/0F38/0F3A AVX decode + emit | upstream `92ec056a6b 1d0efbdb35 03b4588070 d1c1a4222c ce4fcb9478 6bbeb98d10 a64eee3ab4 7906847768 d4af67a27a 16fc5726a6 aba2b8ecb9 7170a17ec3 f8d19eec0d` |
| New helpers in `ops_sse.h`/`ops_sse_header.h` | same series (files become upstream `f8d19eec0d` verbatim, plus Phase 1's 3-line `dh_is_signed` fix) |
| Later upstream fixes (apply as patches) | `0d4bcac3ca` xmm_regs[-1] OOB, `c0a6665c3c`, `3d304620ec` unary op size, `9ad2ba6e8e` **BZHI** (Phase 2 regression, see below), `2b55e479e6` VCOMI size, `056d649007` **VZEROALL cleared xmm_t0 instead of xmm_regs**, `8bf171c2d1` |
| Later upstream fixes (hand backports, `backfix_upstream.py`) | `feea87cd6b` VINSERTx128 mem operand is 128-bit, `2eb8d97343` VMOVD/VMOVQ have no VEX.L=1 form, `ebd9ea2947` VPERMILPS prefix, `ac63755b20` VSIB index 4 (+ translate.c side), `da7c95920d` SSE/SSE2 feature-check bits |
| Unicorn-local strictness (`backfix_upstream.py`, marked `Unicorn:`) | S1 VEX has no MMX forms (upstream executes them; VEX.L=1 `vpinsrw` pp=00 **aborted the host** via `assert(vec_len == 16)`); S3 class-3 scalar ops with VEX.L=1 give `#UD` (upstream's `gen_VROUNDSx` **asserts `!vex_l`: host abort**; VMOVSS/SD retagged class 3, which in 7.2 only affects alignment for class 1); S4 VEX map1 bytes 0x38/0x3A are not escapes |
| `emit.c.inc` port fixes (`port_decoder3.py`) | calls through SSEFunc pointer params, token-pasted helpers in macros, gvec callbacks take `TCGContext *`, `op_ptr`/`tcg_constant8u_i32`/`make_imm8u_xmm_vec` take `tcg_ctx`; `gen_VZEROALL` uses a per-register `tcg_gen_gvec_dup_imm` (QEMU 5.0 has no `helper_memset`, and gvec `oprsz` is capped at 256 bytes: a single 1024-byte dup **asserts in `simd_desc`**) |
| `translate.c` | `gen_sty_env_A0` (mirrors the fork's `gen_ldy_env_A0`: alignment ignored, like all fork SSE loads/stores); `gen_lea_modrm_0(..., bool is_vsib)` at all 9 call sites |
| `cpu.h` | `xmm_regs`/`xmm_t0` get `QEMU_ALIGNED(16)` (upstream has this). **Without it, the first gvec op (`vpxor`) segfaults the host**: 5.0's TCG backend emits aligned `vmovdqa` on `env+0x378`. |
| `cpu.c` (Phase 0b) | `CPUID_EXT_AVX` added to `TCG_EXT_FEATURES`, `CPUID_7_0_EBX_AVX2` to `TCG_7_0_EBX_FEATURES` (FMA/F16C stay filtered). The default model (Haswell) then reports AVX/AVX2/OSXSAVE; reset derives XCR0=7 from the filtered features, and `HF_AVX_EN` syncs via `cpu_x86_update_cr4`. No XSAVE-area or XCR0 code changes were needed. |
| Tests | see "Tests" below |

**Phase 2 regression fixed here:** Phase 2 moved BZHI to the new decoder without upstream fix
`9ad2ba6e8e`. At HEAD `1e5d15c1`, `bzhi` with index ≥ operand size clears the top bit and
leaves CF=0 (native: result=src, CF=1). BMI2 is already advertised, so this is live today.

### Implementation steps (exact, no improvisation)

1. Needs a full, non-shallow QEMU clone: `git clone https://github.com/qemu/qemu <Q>` (must
   contain `1d0b926150`, `b98f886c8f`, `f8d19eec0d` and the fix commits above).
2. Save the six files in "Scripts" below into one directory `<SC>`, byte-exact. Check them with
   `md5sum` against the listed values (the driver re-checks them and STOPs on mismatch).
3. From a clean `avx-tcg-backport` checkout at `1e5d15c1c134e803f62bc76192f3e1b38958fdbf`:
   `bash <SC>/apply_phase3a.sh /home/chry/unicorn <Q> <SC>`
   It must end with `Phase 0b+3a applied; all stage checksums match.`
4. Build and test:
   `cmake -B build -DUNICORN_ARCH=x86 -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j && (cd build && ctest)`
   then `cmake -B build-all -DCMAKE_BUILD_TYPE=Debug && cmake --build build-all -j && (cd build-all && ctest)`.
5. Do not commit (the user reviews and commits).

### STOP-and-report conditions

- Any `STOP [...]` line from the driver: stage 0 means wrong HEAD, a dirty tree or a script
  md5 mismatch; stage 1/2a/2b means upstream patch drift; stage 3/4/5 means port/glue drift.
  Report the line verbatim, and do not hand-edit.
- Any compiler error, or any new warning in `qemu/target/i386/*` (known pre-existing warnings:
  `glib_compat.c` discarded-qualifiers, and `test_m68k.c` pointer-sign in the multi-arch build).
- x86-only build: `ctest` must show **only** `test_mem` failing, with
  `test_mem_read_and_write_large_memory_block` → `free(): invalid pointer`. **This is
  pre-existing at HEAD** (the test opens `UC_ARCH_ARM64` in an x86-only build) and it passes in
  the multi-arch build. Any other failure means STOP.
- Multi-arch `ctest` must be 12/12. `test_x86` must print `SUCCESS: All unit tests have passed.` (66 tests).

### Expected checksums (also embedded in the driver)

| Stage | File | md5 |
|---|---|---|
| 0 (HEAD) | decode-new.c.inc / decode-new.h / emit.c.inc | `e9233e3519095701bdfa55c29114e7ec` / `1ab3d5e9c7a3604c0d5792147bc8c0cf` / `229a586ee5202fa08e114c5818902ee9` |
| 0 (HEAD) | ops_sse.h / ops_sse_header.h | `741713052a6a716c46f3a79d29f7a9c7` / `530f80e7d40e39284cd3ff335f29d661` |
| 0 (HEAD) | translate.c / cpu.c / cpu.h / test_x86.c | `afe6c2b1476c6efa9f9c1ec8d5135b74` / `712d637f74148c46e0dd501ae1005fc7` / `bf80480aa87f380555993e009cac877f` / `5de627f79c72d9b220f32e819d988521` |
| 1 upstream-side base | decode-new.c.inc / .h / emit.c.inc | `cafa0f6fa18b8bbd4c338728d343e595` / `471f104f4f921c29ccc4a7f4097502c8` / `4fcbd5422e28e12b7fbf599924d6ec18` |
| 2a after series | emit.c.inc (others == upstream f8d19eec0d) | `811539c143577225c6fac2a1402c3425` |
| 2b after fixes+backfix | decode-new.c.inc / .h / emit.c.inc | `e331903620f60baa81eb815367cac2e3` / `8699450399d733f80321faa07eb0fa64` / `6c4cbcb4f3baf5c6d93ab96a4c0317d0` |
| 2b | ops_sse.h / ops_sse_header.h | `f7019e39297e4016f4e1d5b0157d9653` / `05b061b94ca0c764382e5748b98c836d` |
| 3 final fork | decode-new.c.inc / decode-new.h / emit.c.inc | `f7b23222b6518a0908873a55b4b89b0a` / `72d0841735c081094a30758a8e8ce049` / `d305e84e43c94d81008c99a8f2eba93e` |
| 3 final fork | ops_sse.h / ops_sse_header.h | `f7019e39297e4016f4e1d5b0157d9653` / `ae8e69655cf8a640f1f3c3a724d0b42a` |
| 4 final fork | translate.c / cpu.c / cpu.h | `78132c037460f686f284d502af9f4709` / `1cfe457f826162c54f4d156470eb3778` / `1a55efc13baca8547d54186b1cc62b23` |
| 5 final | tests/unit/test_x86.c | `96f85858691ccaf785a3f39db459618f` |

Resulting diff: 9 files, +3124 / −94.

### Tests (all added by `addtests_phase3a.py`; the 6 new ones fail at HEAD and pass after)

- `test_x86_avx_cpuid_xcr0`: CPUID.1:ECX AVX=1, OSXSAVE=1, FMA=0, F16C=0; CPUID.7:EBX AVX2=1; XGETBV(0)=7.
- `test_x86_avx_golden`: the msvcrt/ntdll/vcruntime140 subset (vmovdqu ymm load, vpcmpeqb/w ymm,
  vpmovmskb ymm, 3-operand vpxor xmm (upper zeroing) and ymm, vmovups xmm, vinsertf128 mem,
  vmovaps ymm, legacy pxor keeping YMM upper bits, vmovups/vmovaps/vmovdqa/vmovntdq ymm stores).
  Expected values were captured on native AVX2 (i7-14700) by running the identical bytes from an RWX page.
- `test_x86_avx_vzeroupper_vzeroall`, `test_x86_avx_vinsertf128_repro` (the original
  `c4 e3 7d 18 c0 01` repro, now `UC_ERR_OK`).
- `test_x86_bzhi_index_ge_size` (native-captured), `test_x86_vex_invalid_forms_no_abort`
  (the S1/S3/S4 encodings return `UC_ERR_INSN_INVALID` instead of aborting).
- Rewritten because their premise is now false: `test_x86_invalid_vex_l` (VEX.256 vmovdqu is
  valid now, so it uses `vmovd xmm1, ecx` with VEX.L=1, which is `#UD` on hardware), and
  `test_x86_vex_unimplemented_is_invalid` (vpxor is implemented now, so it uses
  `vfmadd231ps`, which stays `#UD` until Phase 5).

### Differential sweep: what was verified beyond the unit tests

`vexsweep.c` + `vexsweep_stub.S` (below; optional, needs an AVX2 host) run every 3-byte-VEX
encoding across map {0F, 0F38, 0F3A} × opcode 0-255 × pp × L × W × {reg, [rax]} × vvvv∈{0,1}
(49,152 cases). Each case runs natively and in Unicorn from identical state, then compares
validity, YMM0-15, RAX/RCX/RDX/RBX/RSI/RDI, arithmetic flags, MXCSR and 8 KiB of memory.
It found the host-abort and segfault bugs listed above, all of which are now fixed.
Run it with `for m in 1 2 3; do for lo in 00 10 … f0; do ./vexsweep $m $lo $((lo+0xf)); done; done`.
It is slow: `#UD` in Unicorn costs about 1 s per case (use `xargs -P`; one map-1 chunk takes about 15 min).

**Final-tree result:** 49,152 cases, 3,060 executed identically on both sides with full state
equality, **0 host aborts**. The residual mismatches are all in the known classes below.

Known residual divergences (for Phase 3b/6; none affects the msvcrt/ntdll/vcruntime140 subset):

1. **Feature-gated (intended):** FMA (0F38 96-BF), F16C (0F38 13, 0F3A 1D), AVX-VNNI
   (0F38 50-53), GFNI, VAES/VPCLMULQDQ-256 are `#UD` in Unicorn and valid on the host.
   VLDMXCSR/VSTMXCSR (`57f6bba023`) are not ported yet.
2. **LIG policy (intended, hard-fail):** scalar ops (vaddss/sd … vmaxss/sd, vcvt*, vcomis*,
   vroundss/sd, vmovss/sd, vcmpss/sd) with VEX.L=1 are `#UD` in Unicorn; hardware ignores L.
   Compilers emit L=0. This matches upstream QEMU's REPScalar policy.
3. **Over-acceptance, inherited from upstream 7.2 permissive tables** (hardware `#UD`, Unicorn
   executes): VEX.W not validated for W0-only ops (0F38 0C-0F/16/18-1A/2C-2F/36/46/58/59/5A/78/79,
   0F3A 00-06/18/19/38/39/46/4A-4C; upstream fixes this in `e000687f12`+`183e6679e3`, which are
   built on a later table refactor); vvvv≠1111 accepted for vmovss/sd memory forms and RORX;
   66 0F 12/16 register forms, 0F 13 L=1, 66 0F E7 register form, SSE4.1 blendv (0F38 10/14/15)
   under VEX, 66 0F 7E with L=1 (**still unfixed even in upstream v11**), and VZEROUPPER/ALL
   with a 66/F3/F2 prefix. These encodings do not come from compilers. They matter as an
   anti-emulation fingerprint, so they are a good Phase 6 target (the sweep is the acceptance test).
4. **Implementation-defined results:** vrcpps/vrsqrtps/ss (0F 52/53) are hardware approximations
   and QEMU computes them exactly. ANDN/BEXTR leave PF (architecturally undefined) differently.
5. **Pre-existing, not a regression:** MXCSR sticky exception flags are never reported by
   `uc_reg_read(UC_X86_REG_MXCSR)`. HEAD reads `0` after an inexact legacy `divss`. The
   QEMU 5.0 base lacks `update_mxcsr_from_sse_status`.
6. Alignment `#GP` for vmovaps/vmovdqa is not enforced, the same as the fork's legacy SSE
   (Phase 6 item). `gen_sty_env_A0` deliberately mirrors this.

### Open questions for the user (answer before or after dispatch; defaults are what the spec does)

- **Scope:** the spec ports the whole upstream VEX series (about 300 instructions, incl. most of
  planned 3b and the decoder half of 4) rather than only the msvcrt subset, because it is the
  mechanical, zero-fuzz path. Only the msvcrt subset plus the sweep's 3,060 identical cases are
  value-verified. Is the larger surface acceptable in one commit, as opposed to hiding
  unverified opcodes behind `#UD`?
- **Unicorn-local strictness S1/S3/S4** diverge from upstream QEMU's behavior on purpose (two
  of them fix host aborts). Keep them?
- **LIG with L=1 → `#UD`** (hardware executes it). Accept as policy, or invest in real LIG support later?
- The residual over-acceptance class (item 3) needs a decision: Phase 6, or a follow-up 3b
  commit that backports `e000687f12`/`183e6679e3`-equivalent checks?
- WindowsEngine reports host `PF_AVX*` regardless of Unicorn's CPUID. After this phase the two
  agree on AVX/AVX2, but the host's FMA/F16C/AVX-VNNI are still reported by WindowsEngine while
  Unicorn `#UD`s them (ucrtbase uses `vfmadd*sd` 400+ times). WindowsEngine's
  `IsProcessorFeaturePresent`/CPUID hook should be clamped to what Unicorn implements, or
  Phase 5 (FMA/F16C) should be pulled forward.

### Scripts (save byte-exact; md5 in each heading)

#### `apply_phase3a.sh` — md5 `16567825ad75f8560b3430e8a2a44c8f`

```bash
#!/usr/bin/env bash
# Phase 0b+3a driver: AVX/AVX2 VEX coverage for the Unicorn avx-tcg-backport branch.
# usage: apply_phase3a.sh <unicorn_root> <qemu_git_dir> <scripts_dir>
#   <unicorn_root>  clean checkout of avx-tcg-backport at 1e5d15c1 (Phase 2)
#   <qemu_git_dir>  full (non-shallow) clone of https://github.com/qemu/qemu
#   <scripts_dir>   directory holding ucrewrite.py port_decoder3.py backfix_upstream.py
#                   glue_phase3a.py addtests_phase3a.py phase3a_tests.c
# Every stage is checked against expected md5sums; any mismatch prints STOP and exits non-zero.
set -euo pipefail
UC=$(realpath "$1"); Q=$(realpath "$2"); SC=$(realpath "$3")
T=$UC/qemu/target/i386
W=$(mktemp -d); mkdir -p "$W/up"

check() { # <file> <expected md5> <stage>
    local got; got=$(md5sum "$1" | cut -d' ' -f1)
    if [ "$got" != "$2" ]; then echo "STOP [$3]: $1: md5 $got, expected $2"; exit 1; fi
}
upstream_patch() { # <commit> : apply that commit's changes to the upstream-side files in $W/up
    git -C "$Q" diff "$1~1" "$1" -- target/i386/tcg/decode-new.c.inc target/i386/tcg/decode-new.h \
        target/i386/tcg/emit.c.inc target/i386/ops_sse.h target/i386/ops_sse_header.h \
      | sed -e 's#^\(---\|+++\) \([ab]\)/target/i386/tcg/#\1 \2/#' -e 's#^\(---\|+++\) \([ab]\)/target/i386/#\1 \2/#' \
      > "$W/$1.patch"
    (cd "$W/up" && patch -p1 --no-backup-if-mismatch --reject-file=- < "../$1.patch" > "../$1.log") \
        || { echo "STOP [patch $1]: did not apply"; cat "$W/$1.log"; exit 1; }
}

# --- Stage 0: preconditions. ---
[ "$(git -C "$UC" rev-parse HEAD)" = 1e5d15c1c134e803f62bc76192f3e1b38958fdbf ] || { echo "STOP [0]: unicorn HEAD is not 1e5d15c1"; exit 1; }
[ -z "$(git -C "$UC" status --porcelain --untracked-files=no)" ] || { echo "STOP [0]: unicorn tree has tracked modifications"; exit 1; }
check "$SC/ucrewrite.py"         2a05d6e97ff6a9bba4f728afae6deb93 0
check "$SC/port_decoder3.py"     eea049d14324d21e338059549732da1f 0
check "$SC/backfix_upstream.py"  d4321a359d2335c10e75eff3f08528f5 0
check "$SC/glue_phase3a.py"      e6f3a7f60fe581d3ba223424531fa904 0
check "$SC/addtests_phase3a.py"  092d5d987e739c3e46410d564a3f766d 0
check "$SC/phase3a_tests.c"      5a19f897449d2d3804eb0e68779e5eba 0
check "$T/decode-new.c.inc"      e9233e3519095701bdfa55c29114e7ec 0
check "$T/decode-new.h"          1ab3d5e9c7a3604c0d5792147bc8c0cf 0
check "$T/emit.c.inc"            229a586ee5202fa08e114c5818902ee9 0
check "$T/ops_sse.h"             741713052a6a716c46f3a79d29f7a9c7 0
check "$T/ops_sse_header.h"      530f80e7d40e39284cd3ff335f29d661 0
check "$T/translate.c"           afe6c2b1476c6efa9f9c1ec8d5135b74 0
check "$T/cpu.c"                 712d637f74148c46e0dd501ae1005fc7 0
check "$T/cpu.h"                 bf80480aa87f380555993e009cac877f 0
check "$UC/tests/unit/test_x86.c" 5de627f79c72d9b220f32e819d988521 0

# --- Stage 1: rebuild Phase 2's upstream-side inputs (1d0b926150 + BEXTR/BLS* flag fixes). ---
for f in decode-new.c.inc decode-new.h emit.c.inc; do
    git -C "$Q" show 1d0b926150:target/i386/tcg/$f > "$W/up/$f"
done
git -C "$Q" show b98f886c8f:target/i386/ops_sse.h > "$W/up/ops_sse.h"
git -C "$Q" show b98f886c8f:target/i386/ops_sse_header.h > "$W/up/ops_sse_header.h"
upstream_patch b14c009897
upstream_patch 99282098dc
check "$W/up/decode-new.c.inc"  cafa0f6fa18b8bbd4c338728d343e595 1
check "$W/up/decode-new.h"      471f104f4f921c29ccc4a7f4097502c8 1
check "$W/up/emit.c.inc"        4fcbd5422e28e12b7fbf599924d6ec18 1
check "$W/up/ops_sse.h"         741713052a6a716c46f3a79d29f7a9c7 1

# --- Stage 2: upstream 0F/0F38/0F3A AVX series, later fixes, and hand backports. ---
SERIES="92ec056a6b 1d0efbdb35 03b4588070 d1c1a4222c ce4fcb9478 6bbeb98d10 a64eee3ab4 7906847768 d4af67a27a 16fc5726a6 aba2b8ecb9 7170a17ec3 f8d19eec0d"
for c in $SERIES; do upstream_patch "$c"; done
for f in decode-new.c.inc decode-new.h ops_sse.h ops_sse_header.h; do
    case $f in ops_*) p=target/i386/$f ;; *) p=target/i386/tcg/$f ;; esac
    git -C "$Q" show f8d19eec0d:$p | cmp -s - "$W/up/$f" || { echo "STOP [2a]: $f differs from upstream f8d19eec0d"; exit 1; }
done
check "$W/up/emit.c.inc"        811539c143577225c6fac2a1402c3425 2a
FIXES="0d4bcac3ca c0a6665c3c 3d304620ec 9ad2ba6e8e 2b55e479e6 056d649007 8bf171c2d1"
for c in $FIXES; do upstream_patch "$c"; done
python3 "$SC/backfix_upstream.py" "$W/up"
check "$W/up/decode-new.c.inc"  e331903620f60baa81eb815367cac2e3 2b
check "$W/up/decode-new.h"      8699450399d733f80321faa07eb0fa64 2b
check "$W/up/emit.c.inc"        6c4cbcb4f3baf5c6d93ab96a4c0317d0 2b
check "$W/up/ops_sse.h"         f7019e39297e4016f4e1d5b0157d9653 2b
check "$W/up/ops_sse_header.h"  05b061b94ca0c764382e5748b98c836d 2b

# --- Stage 3: tcg_ctx port into the fork. ---
for f in decode-new.c.inc emit.c.inc decode-new.h; do python3 "$SC/port_decoder3.py" "$W/up/$f" "$T/$f"; done
cp "$W/up/ops_sse.h" "$T/ops_sse.h"
sed -e 's/^#define dh_typecode_\(Reg\|ZMMReg\|MMXReg\) dh_typecode_ptr$/#define dh_is_signed_\1 dh_is_signed_ptr/' \
    "$W/up/ops_sse_header.h" > "$T/ops_sse_header.h"
check "$T/decode-new.c.inc"      f7b23222b6518a0908873a55b4b89b0a 3
check "$T/decode-new.h"          72d0841735c081094a30758a8e8ce049 3
check "$T/emit.c.inc"            d305e84e43c94d81008c99a8f2eba93e 3
check "$T/ops_sse.h"             f7019e39297e4016f4e1d5b0157d9653 3
check "$T/ops_sse_header.h"      ae8e69655cf8a640f1f3c3a724d0b42a 3

# --- Stage 4: translate.c / cpu.c / cpu.h glue (includes Phase 0b CPUID flip). ---
python3 "$SC/glue_phase3a.py" "$UC"
check "$T/translate.c"           78132c037460f686f284d502af9f4709 4
check "$T/cpu.c"                 1cfe457f826162c54f4d156470eb3778 4
check "$T/cpu.h"                 1a55efc13baca8547d54186b1cc62b23 4

# --- Stage 5: tests. ---
python3 "$SC/addtests_phase3a.py" "$UC/tests/unit/test_x86.c" "$SC/phase3a_tests.c"
check "$UC/tests/unit/test_x86.c" 96f85858691ccaf785a3f39db459618f 5

rm -rf "$W"
echo "Phase 0b+3a applied; all stage checksums match."
```

#### `ucrewrite.py` — md5 `2a05d6e97ff6a9bba4f728afae6deb93`

```python
#!/usr/bin/env python3
"""Mechanical vanilla-QEMU -> Unicorn tcg_ctx rewrite for a target/i386 translate.c fragment.

usage: ucrewrite.py in.c out.c
Callers must still add `TCGContext *tcg_ctx = s->uc->tcg_ctx;` to each function that uses tcg_ctx.
"""
import re
import sys

t = open(sys.argv[1]).read()
# SSEFunc_* typedefs take the TCGContext first.
t = re.sub(r'typedef void \(\*(SSEFunc_\w+)\)\(', r'typedef void (*\1)(TCGContext *s, ', t)
# Direct TCG / helper calls.
t = re.sub(r'\b(tcg_gen_\w+|gen_helper_\w+|tcg_temp_\w+|tcg_const_\w+|tcg_constant_\w+|gen_extu)\(',
           r'\1(tcg_ctx, ', t)
# Calls through SSE function pointers: sse_fn_*(...), fn(...), op6->fn[x].opN(...), op7->fn[x].opN(...).
t = re.sub(r'(\bsse_fn_\w+|\bfn|\bop_3dnow|\.op\w*)\(', r'\1(tcg_ctx, ', t)
t = t.replace('(tcg_ctx, )', '(tcg_ctx)')
# QEMU 7 renamed MO_LEQ -> MO_LEUQ.
t = re.sub(r'\bMO_LEUQ\b', 'MO_LEQ', t)
# TCG globals live in TCGContext under Unicorn.
t = re.sub(r'(?<![>\w])cpu_(env|regs|cc_dst|cc_src2|cc_src|cc_op|seg_base|bndl|bndu)\b', r'tcg_ctx->cpu_\1', t)
# Static gen_helper_* wrappers defined inside the fragment: fix their definitions.
t = re.sub(r'^(static void gen_helper_\w+)\(tcg_ctx, ', r'\1(TCGContext *tcg_ctx, ', t, flags=re.M)
open(sys.argv[2], 'w').write(t)
```

#### `port_decoder3.py` — md5 `eea049d14324d21e338059549732da1f`

```python
#!/usr/bin/env python3
"""Phase 3a: port upstream QEMU f8d19eec0d(+fixes) decode-new.h / decode-new.c.inc / emit.c.inc to Unicorn.

Superset of Phase 2's port_decoder.py (identical output on Phase 2 inputs is NOT required).

usage: port_decoder.py <in_file> <out_file>
Applies ucrewrite.py (tcg_ctx threading), expands GNU case ranges (Unicorn builds with MSVC),
and declares `TCGContext *tcg_ctx = s->uc->tcg_ctx;` at the top of every function that has a
`DisasContext *s` parameter and uses tcg_ctx.
"""
import re
import subprocess
import sys
import tempfile

here = __file__.rsplit('/', 1)[0]
src, dst = sys.argv[1:3]
tmp = tempfile.mkdtemp()
subprocess.check_call([sys.executable, here + '/ucrewrite.py', src, tmp + '/o.c'])
t = open(tmp + '/o.c').read()

# --- GNU case ranges -> explicit labels. ---
RANGES = {
    'case X86_TYPE_0 ... X86_TYPE_7:':
        ' '.join('case X86_TYPE_%d:' % i for i in range(8)),
    'case X86_TYPE_ES ... X86_TYPE_GS:':
        'case X86_TYPE_ES: case X86_TYPE_CS: case X86_TYPE_SS: case X86_TYPE_DS: case X86_TYPE_FS: case X86_TYPE_GS:',
    'case 0x40 ... 0x4f:':
        ' '.join('case 0x%02x:' % i for i in range(0x40, 0x50)),
}
for k, v in RANGES.items():
    t = t.replace(k, v)
assert not re.search(r'case [^:\n]+\.\.\.', t), 'unexpanded case range'

# --- Phase 3a: calls through SSEFunc_* pointer parameters/locals take tcg_ctx first. ---
names = set(re.findall(r'\bSSEFunc_\w+\s+(\w+)\b', t)) - {'fn'}
for n in sorted(names):
    t = re.sub(r'(?<![\w.>])' + n + r'\(', n + '(tcg_ctx, ', t)
t = re.sub(r'(gen_helper_cmp_funcs\[\w+\]\[\w+\])\(', r'\1(tcg_ctx, ', t)
# Token-pasted helper calls inside macros.
t = re.sub(r'(gen_helper_##\w+##\w*)\(', r'\1(tcg_ctx, ', t)
t = re.sub(r'(?m)^(    func)\(__VA_ARGS__', r'\1(tcg_ctx, __VA_ARGS__', t)
# Local helpers without a DisasContext: take tcg_ctx as first parameter.
for n in ('tcg_constant8u_i32', 'op_ptr', 'make_imm8u_xmm_vec'):
    t = re.sub(r'(?<![\w])' + n + r'\(', n + '(tcg_ctx, ', t)
    t = re.sub(r'(?m)^(static (?:inline )?TCGv_\w+ ' + n + r')\(tcg_ctx, ', r'\1(TCGContext *tcg_ctx, ', t)
# GVecGen callbacks: fork's fni8/fniv take TCGContext first.
t = re.sub(r'(?m)^(static void gen_\w+_(?:i32|i64|vec))\((?=(?:unsigned vece, )?TCGv_)', r'\1(TCGContext *tcg_ctx, ', t)
# Macro-generated gen_* bodies that use tcg_ctx need a local declaration.
t = re.sub(r'(static void gen_##u\w*name\(DisasContext \*s, CPUX86State \*env, X86DecodedInsn \*decode\) *\\\n\{ *\\\n)'
           r'(?=(?:[^\n]*\\\n)*?[^\n]*tcg_ctx)',
           lambda m: m.group(1) + '    TCGContext *tcg_ctx = s->uc->tcg_ctx;                                         \\\n', t)

# --- Declare tcg_ctx in functions that need it. ---
out = []
lines = t.split('\n')
i = 0
func_re = re.compile(r'^(static\s+)?(inline\s+)?[\w\s\*]+\b\w+\((.*)$')
while i < len(lines):
    line = lines[i]
    out.append(line)
    m = func_re.match(line)
    if m and not line.rstrip().endswith(';') and not line.startswith('#'):
        # Collect the full signature up to the opening brace line.
        sig = [line]
        j = i
        while not lines[j].rstrip().endswith(')') and j + 1 < len(lines) and lines[j + 1] != '{':
            j += 1
            sig.append(lines[j])
        if j + 1 < len(lines) and lines[j + 1] == '{':
            # Find function body end (next line that is exactly '}').
            k = j + 2
            while k < len(lines) and lines[k] != '}':
                k += 1
            body = '\n'.join(lines[j + 2:k])
            sigtxt = ' '.join(sig)
            if 'tcg_ctx' in body and 'DisasContext *s' in sigtxt and 'TCGContext *tcg_ctx' not in sigtxt:
                out.extend(lines[i + 1:j + 2])
                out.append('    TCGContext *tcg_ctx = s->uc->tcg_ctx;')
                i = j + 2
                continue
    i += 1
t = '\n'.join(out)

# --- Unicorn-specific fixups (each anchor must exist exactly once in the file it applies to). ---
FIXUPS = [
    # QEMU 5.0 gen_exception() still takes the faulting EIP.
    ('    gen_exception(s, EXCP07_PREX);\n',
     '    gen_exception(s, EXCP07_PREX, s->pc_start - s->cs_base);\n'),
    # Functions without a DisasContext: thread tcg_ctx explicitly.
    ('static void decode_temp_free(X86DecodedOp *op)\n',
     'static void decode_temp_free(TCGContext *tcg_ctx, X86DecodedOp *op)\n'),
    ('static void decode_temps_free(X86DecodedInsn *decode)\n{\n'
     '    decode_temp_free(&decode->op[0]);\n'
     '    decode_temp_free(&decode->op[1]);\n'
     '    decode_temp_free(&decode->op[2]);\n',
     'static void decode_temps_free(TCGContext *tcg_ctx, X86DecodedInsn *decode)\n{\n'
     '    decode_temp_free(tcg_ctx, &decode->op[0]);\n'
     '    decode_temp_free(tcg_ctx, &decode->op[1]);\n'
     '    decode_temp_free(tcg_ctx, &decode->op[2]);\n'),
    ('    decode_temps_free(&decode);\n', '    decode_temps_free(tcg_ctx, &decode);\n'),
    # BLSI sets CF = (src != 0), the inverse of BLSMSK/BLSR. Upstream fixed this in 83a3a20e59 with a
    # new CC_OP_BLSI*; here keep CC_OP_BMILG (CF = cc_src == 0) and store cc_src = (src == 0) instead.
    ('    tcg_gen_mov_tl(tcg_ctx, tcg_ctx->cpu_cc_src, s->T0);\n    tcg_gen_neg_tl(tcg_ctx, s->T1, s->T0);\n',
     '    tcg_gen_setcondi_tl(tcg_ctx, TCG_COND_EQ, tcg_ctx->cpu_cc_src, s->T0, 0);\n    tcg_gen_neg_tl(tcg_ctx, s->T1, s->T0);\n'),
    # QEMU 5.0 MemOp has no 128/256-bit sizes. Decoder-internal operand-size tags only:
    # they select gen_ldo/gen_ldy paths and are never passed to tcg_gen_qemu_ld/st.
    ('typedef enum X86OpType {',
     '/* Unicorn: QEMU 5.0 MemOp lacks these; decoder-internal size tags, never used as a memory MemOp. */\n'
     '#ifndef MO_128\n#define MO_128 ((MemOp)4)\n#define MO_256 ((MemOp)5)\n#endif\n\n'
     'typedef enum X86OpType {'),
    # Phase 3a: no helper_memset in QEMU 5.0's tcg-runtime; zero xmm_regs with a gvec dup instead.
    ('    TCGv_ptr ptr = tcg_temp_new_ptr(tcg_ctx);\n\n'
     '    tcg_gen_addi_ptr(tcg_ctx, ptr, tcg_ctx->cpu_env, offsetof(CPUX86State, xmm_regs));\n'
     '    gen_helper_memset(tcg_ctx, ptr, ptr, tcg_constant_i32(tcg_ctx, 0),\n'
     '                      tcg_constant_ptr(tcg_ctx, CPU_NB_REGS * sizeof(ZMMReg)));\n'
     '    tcg_temp_free_ptr(tcg_ctx, ptr);\n',
     '    int i;\n\n'
     '    /* Unicorn: QEMU 5.0 has no helper_memset and gvec is limited to 256 bytes; clear per register. */\n'
     '    for (i = 0; i < CPU_NB_REGS; i++) {\n'
     '        tcg_gen_gvec_dup_imm(tcg_ctx, MO_64, offsetof(CPUX86State, xmm_regs[i]),\n'
     '                             sizeof(ZMMReg), sizeof(ZMMReg), 0);\n'
     '    }\n'),
]
for old, new in FIXUPS:
    c = t.count(old)
    assert c <= 1, old
    t = t.replace(old, new)
open(dst, 'w').write(t)
```

#### `backfix_upstream.py` — md5 `d4321a359d2335c10e75eff3f08528f5`

```python
#!/usr/bin/env python3
"""Phase 3a: backport later upstream decoder fixes whose patches do not apply to the f8d19eec0d-era tables.

usage: backfix_upstream.py <upstream_side_dir>   (edits decode-new.c.inc in place; every anchor must match exactly once)
"""
import sys
p = sys.argv[1] + '/decode-new.c.inc'
t = open(p).read()
FIXES = [
    # feea87cd6b: VINSERTx128's third operand is 128 bits (memory form must not read 32 bytes).
    ('    [0x18] = X86_OP_ENTRY4(VINSERTx128,  V,qq, H,qq, W,qq, vex6 cpuid(AVX) p_66),\n',
     '    [0x18] = X86_OP_ENTRY4(VINSERTx128,  V,qq, H,qq, W,dq, vex6 cpuid(AVX) p_66),\n'),
    ('    [0x38] = X86_OP_ENTRY4(VINSERTx128,  V,qq, H,qq, W,qq, vex6 cpuid(AVX2) p_66),\n',
     '    [0x38] = X86_OP_ENTRY4(VINSERTx128,  V,qq, H,qq, W,dq, vex6 cpuid(AVX2) p_66),\n'),
    # 2eb8d97343: VMOVD/VMOVQ/VMOVLPD (exception class 5) have no VEX.L=1 form (#UD on hardware).
    ('        X86_OP_ENTRY3(MOVQ,       V,x, None,None, W,q, vex5),  /* wrong dest Vy on SDM! */\n',
     '        X86_OP_ENTRY3(MOVQ,       V,dq,None,None, W,q, vex5),  /* wrong dest Vq on SDM! */\n'),
    ('        X86_OP_ENTRY3(MOVQ,    W,x,  None, None, V,q, vex5),\n',
     '        X86_OP_ENTRY3(MOVQ,    W,dq, None, None, V,q, vex5),\n'),
    ('        X86_OP_ENTRY3(VMOVLPx,   W,x,  H,x,        U,q,  vex4), /* MOVLPD */\n',
     '        X86_OP_ENTRY3(VMOVLPx,   W,dq, H,dq,       U,q,  vex4), /* MOVLPD */\n'),
    ('    [0x6e] = X86_OP_ENTRY3(MOVD_to,    V,x, None,None, E,y, vex5 mmx p_00_66),  /* wrong dest Vy on SDM! */\n',
     '    [0x6e] = X86_OP_ENTRY3(MOVD_to,    V,dq,None,None, E,y, vex5 mmx p_00_66),  /* wrong dest Vy on SDM! */\n'),
    # ebd9ea2947: VPERMILPS (0F38 0C) only exists with the 66 prefix.
    ('    [0x0c] = X86_OP_ENTRY3(VPERMILPS, V,x,        H,x,  W,x,  vex4 cpuid(AVX) p_00_66),\n',
     '    [0x0c] = X86_OP_ENTRY3(VPERMILPS, V,x,        H,x,  W,x,  vex4 cpuid(AVX) p_66),\n'),
    # ac63755b20: with VSIB, SIB.index == 4 selects xmm4/ymm4, it is not "no index".
    ('        decode->mem = gen_lea_modrm_0(env, s, get_modrm(s, env));\n',
     '        decode->mem = gen_lea_modrm_0(env, s, get_modrm(s, env),\n'
     '                                      decode->e.vex_class == 12);\n'),
    # da7c95920d: SSE/SSE2 are CPUID.1:EDX bits, not ECX.
    ('        return (s->cpuid_ext_features & CPUID_SSE);\n',
     '        return (s->cpuid_features & CPUID_SSE);\n'),
    ('        return (s->cpuid_ext_features & CPUID_SSE2);\n',
     '        return (s->cpuid_features & CPUID_SSE2);\n'),
    # --- Unicorn-local strictness (not upstream; found by the native-vs-Unicorn VEX sweep). ---
    # S1: VEX has no MMX forms. Upstream executes VEX.pp=00 MMX-capable opcodes as MMX, and
    # VEX.L=1 PINSRW (0F C4) then hits assert(vec_len == 16) in gen_pinsr (host abort).
    ('    case X86_SPECIAL_MMX:\n'
     '        if (!(s->prefix & (PREFIX_REPZ | PREFIX_REPNZ | PREFIX_DATA))) {\n'
     '            gen_helper_enter_mmx(cpu_env);\n',
     '    case X86_SPECIAL_MMX:\n'
     '        if (!(s->prefix & (PREFIX_REPZ | PREFIX_REPNZ | PREFIX_DATA))) {\n'
     '            if (s->prefix & PREFIX_VEX) {\n'
     '                /* Unicorn: there are no VEX-encoded MMX instructions.  */\n'
     '                goto illegal_op;\n'
     '            }\n'
     '            gen_helper_enter_mmx(cpu_env);\n'),
    # S3: scalar (class 3) instructions are LIG on hardware.  Like REPScalar above, reject VEX.L=1
    # instead of generating a 256-bit form (gen_VROUNDSx asserts !vex_l: host abort).
    ('    /* TODO: instructions that require VEX.W=0 (Table 2-16) */\n',
     '    /* TODO: instructions that require VEX.W=0 (Table 2-16) */\n'
     '\n'
     '    /*\n'
     '     * Unicorn: scalar instructions ignore VEX.L on hardware; reject VEX.L=1 like\n'
     '     * X86_VEX_REPScalar does, rather than emitting a 256-bit form.\n'
     '     */\n'
     '    if ((s->prefix & PREFIX_VEX) && e->vex_class == 3 && s->vex_l) {\n'
     '        goto illegal;\n'
     '    }\n'),
    # S3b: VMOVSS/VMOVSD are scalar too: tag them class 3 (class only affects alignment for class 1).
    ('        X86_OP_ENTRY3(VMOVSS,  V,x,  H,x,       W,x, vex4),\n'
     '        X86_OP_ENTRY3(VMOVLPx, V,x,  H,x,       W,x, vex4), /* MOVSD */\n',
     '        X86_OP_ENTRY3(VMOVSS,  V,x,  H,x,       W,x, vex3),\n'
     '        X86_OP_ENTRY3(VMOVLPx, V,x,  H,x,       W,x, vex3), /* MOVSD */\n'),
    ('        X86_OP_ENTRY3(VMOVSS_ld,  V,x,  H,x,       M,ss, vex4),\n'
     '        X86_OP_ENTRY3(VMOVSD_ld,  V,x,  H,x,       M,sd, vex4),\n',
     '        X86_OP_ENTRY3(VMOVSS_ld,  V,x,  H,x,       M,ss, vex3),\n'
     '        X86_OP_ENTRY3(VMOVSD_ld,  V,x,  H,x,       M,sd, vex3),\n'),
    ('        X86_OP_ENTRY3(VMOVSS,  W,x,  H,x,       V,x, vex4),\n'
     '        X86_OP_ENTRY3(VMOVLPx, W,x,  H,x,       V,q, vex4), /* MOVSD */\n',
     '        X86_OP_ENTRY3(VMOVSS,  W,x,  H,x,       V,x, vex3),\n'
     '        X86_OP_ENTRY3(VMOVLPx, W,x,  H,x,       V,q, vex3), /* MOVSD */\n'),
    ('        X86_OP_ENTRY3(VMOVSS_st,  M,ss, None,None, V,x, vex4),\n'
     '        X86_OP_ENTRY3(VMOVLPx_st, M,sd, None,None, V,x, vex4), /* MOVSD */\n',
     '        X86_OP_ENTRY3(VMOVSS_st,  M,ss, None,None, V,x, vex3),\n'
     '        X86_OP_ENTRY3(VMOVLPx_st, M,sd, None,None, V,x, vex3), /* MOVSD */\n'),
    # S4: under VEX the 0F38/0F3A maps are selected by VEX.mmmmm; 38/3A are not escapes in map 1.
    ('static void decode_0F(DisasContext *s, CPUX86State *env, X86OpEntry *entry, uint8_t *b)\n'
     '{\n'
     '    *b = x86_ldub_code(env, s);\n',
     'static void decode_0F(DisasContext *s, CPUX86State *env, X86OpEntry *entry, uint8_t *b)\n'
     '{\n'
     '    *b = x86_ldub_code(env, s);\n'
     '    if ((s->prefix & PREFIX_VEX) && (*b == 0x38 || *b == 0x3a)) {\n'
     '        /* Unicorn: not an escape byte in VEX map 1.  */\n'
     '        memset(entry, 0, sizeof(*entry));\n'
     '        return;\n'
     '    }\n'),
]
for old, new in FIXES:
    n = t.count(old)
    if n != 1:
        sys.exit('STOP: backfix anchor matched %d times: %r' % (n, old))
    t = t.replace(old, new)
open(p, 'w').write(t)
```

#### `glue_phase3a.py` — md5 `e6f3a7f60fe581d3ba223424531fa904`

```python
#!/usr/bin/env python3
"""Phase 0b+3a glue for translate.c / cpu.c. usage: glue_phase3a.py <unicorn_root>"""
import sys
root = sys.argv[1]

def edit(path, pairs):
    p = root + '/' + path
    t = open(p).read()
    for old, new in pairs:
        assert t.count(old) == 1, (path, old[:80])
        t = t.replace(old, new)
    open(p, 'w').write(t)

LDY_END = ('        tcg_gen_st_i64(tcg_ctx, s->tmp1_i64, tcg_ctx->cpu_env,\n'
           '                       offset + offsetof(YMMReg, YMM_Q(i)));\n'
           '    }\n'
           '}\n')
STY = ('\n'
       'static void gen_sty_env_A0(DisasContext *s, int offset, bool align)\n'
       '{\n'
       '    TCGContext *tcg_ctx = s->uc->tcg_ctx;\n'
       '    int mem_index = s->mem_index;\n'
       '    int i;\n'
       '\n'
       '    for (i = 0; i < 4; i++) {\n'
       '        tcg_gen_ld_i64(tcg_ctx, s->tmp1_i64, tcg_ctx->cpu_env,\n'
       '                       offset + offsetof(YMMReg, YMM_Q(i)));\n'
       '        tcg_gen_addi_tl(tcg_ctx, s->tmp0, s->A0, i * 8);\n'
       '        tcg_gen_qemu_st_i64(tcg_ctx, s->tmp1_i64, s->tmp0, mem_index, MO_LEQ);\n'
       '    }\n'
       '}\n')
edit('qemu/target/i386/translate.c', [(LDY_END, LDY_END + STY)])

# --- Phase 0b: advertise AVX and AVX2 (FMA/F16C wait for Phase 5). ---
edit('qemu/target/i386/cpu.c', [
    ('          CPUID_EXT_MOVBE | CPUID_EXT_AES | CPUID_EXT_HYPERVISOR | \\\n'
     '          CPUID_EXT_RDRAND)\n',
     '          CPUID_EXT_MOVBE | CPUID_EXT_AES | CPUID_EXT_HYPERVISOR | \\\n'
     '          CPUID_EXT_RDRAND | CPUID_EXT_AVX)\n'),
    ('          CPUID_EXT_X2APIC, CPUID_EXT_TSC_DEADLINE_TIMER, CPUID_EXT_AVX,\n'
     '          CPUID_EXT_F16C */\n',
     '          CPUID_EXT_X2APIC, CPUID_EXT_TSC_DEADLINE_TIMER,\n'
     '          CPUID_EXT_F16C */\n'),
    ('          CPUID_7_0_EBX_ERMS)\n'
     '          /* missing:\n'
     '          CPUID_7_0_EBX_HLE, CPUID_7_0_EBX_AVX2,\n',
     '          CPUID_7_0_EBX_ERMS | CPUID_7_0_EBX_AVX2)\n'
     '          /* missing:\n'
     '          CPUID_7_0_EBX_HLE,\n'),
])

# --- gvec V128 host ops (aligned vmovdqa) on env offsets need upstream's 16-byte alignment. ---
edit('qemu/target/i386/cpu.h', [
    ('    ZMMReg xmm_regs[CPU_NB_REGS == 8 ? 8 : 32];\n'
     '    ZMMReg xmm_t0;\n',
     '    ZMMReg xmm_regs[CPU_NB_REGS == 8 ? 8 : 32] QEMU_ALIGNED(16);\n'
     '    ZMMReg xmm_t0 QEMU_ALIGNED(16);\n'),
])

# --- ac63755b20 (VSIB index 4) translate.c side: gen_lea_modrm_0 grows is_vsib. ---
p = root + '/qemu/target/i386/translate.c'
t = open(p).read()
old_sig = ('static AddressParts gen_lea_modrm_0(CPUX86State *env, DisasContext *s,\n'
           '                                    int modrm)\n')
assert t.count(old_sig) == 1, 'gen_lea_modrm_0 signature'
t = t.replace(old_sig, 'static AddressParts gen_lea_modrm_0(CPUX86State *env, DisasContext *s,\n'
                       '                                    int modrm, bool is_vsib)\n')
old_idx = ('            index = ((code >> 3) & 7) | REX_X(s);\n'
           '            if (index == 4) {\n')
assert t.count(old_idx) == 1, 'SIB index==4'
t = t.replace(old_idx, '            index = ((code >> 3) & 7) | REX_X(s);\n'
                       '            if (index == 4 && !is_vsib) {\n')
n = t.count('gen_lea_modrm_0(env, s, modrm)')
assert n == 9, ('gen_lea_modrm_0 call sites', n)
t = t.replace('gen_lea_modrm_0(env, s, modrm)', 'gen_lea_modrm_0(env, s, modrm, false)')
open(p, 'w').write(t)
```

#### `addtests_phase3a.py` — md5 `092d5d987e739c3e46410d564a3f766d`

```python
#!/usr/bin/env python3
"""usage: addtests_phase3a.py <tests/unit/test_x86.c> <phase3a_tests.c>"""
import sys
p = sys.argv[1]
t = open(p).read()
g = open(sys.argv[2]).read()

def rep(old, new):
    global t
    n = t.count(old)
    if n != 1:
        sys.exit('STOP: test anchor matched %d times: %r' % (n, old[:70]))
    t = t.replace(old, new)

# VEX.256 vmovdqu is valid now; keep the test on an encoding that really has no VEX.L=1 form.
rep("    /* vmovdqu ymm1, [rcx] */\n    char code[] = {'\\xC5', '\\xFE', '\\x6F', '\\x09'};\n",
    "    /* vmovd xmm1, ecx with VEX.L=1: #UD on real hardware */\n    char code[] = {'\\xC5', '\\xFD', '\\x6E', '\\xC9'};\n")
# vpxor is implemented now; FMA stays unadvertised and undecoded until Phase 5.
rep('// VEX-encoded SSE/AVX is not implemented until Phase 3: it must fault, not execute as legacy SSE.\n',
    '// FMA is not implemented until Phase 5: it must fault, not execute as something else.\n')
rep('    char code[] = "\\xc5\\xf1\\xef\\xc2"; // vpxor xmm0, xmm1, xmm2\n',
    '    char code[] = "\\xc4\\xe2\\x71\\xb8\\xc2"; // vfmadd231ps xmm0, xmm1, xmm2\n')
rep('    TEST_CHECK(out[0] == 0x4444 && out[1] == 0); // untouched (old decoder produced 0x6666)\n',
    '    TEST_CHECK(out[0] == 0x4444 && out[1] == 0); // untouched\n')

names = ['test_x86_avx_cpuid_xcr0', 'test_x86_avx_golden', 'test_x86_avx_vzeroupper_vzeroall',
         'test_x86_avx_vinsertf128_repro', 'test_x86_bzhi_index_ge_size',
         'test_x86_vex_invalid_forms_no_abort']
rep('TEST_LIST = {', g + '\nTEST_LIST = {\n' + ''.join('    {"%s", %s},\n' % (n, n) for n in names))
open(p, 'w').write(t)
```

#### `phase3a_tests.c` — md5 `5a19f897449d2d3804eb0e68779e5eba`

```c
// CPUID must advertise AVX/AVX2 (and OSXSAVE) now that VEX decode backs them; FMA/F16C wait for Phase 5.
static void test_x86_avx_cpuid_xcr0(void)
{
    uc_engine *uc;
    char code[] = "\xb8\x01\x00\x00\x00" // mov eax, 1
                  "\x0f\xa2"             // cpuid
                  "\x41\x89\xc8"         // mov r8d, ecx
                  "\xb8\x07\x00\x00\x00" // mov eax, 7
                  "\x31\xc9"             // xor ecx, ecx
                  "\x0f\xa2"             // cpuid
                  "\x41\x89\xd9"         // mov r9d, ebx
                  "\x31\xc9"             // xor ecx, ecx
                  "\x0f\x01\xd0";        // xgetbv
    uint64_t r8, r9, rax, rdx;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_R8, &r8));
    OK(uc_reg_read(uc, UC_X86_REG_R9, &r9));
    OK(uc_reg_read(uc, UC_X86_REG_RAX, &rax));
    OK(uc_reg_read(uc, UC_X86_REG_RDX, &rdx));

    TEST_CHECK((r8 >> 28) & 1);    // CPUID.1:ECX.AVX
    TEST_CHECK((r8 >> 27) & 1);    // CPUID.1:ECX.OSXSAVE
    TEST_CHECK(!((r8 >> 12) & 1)); // CPUID.1:ECX.FMA (Phase 5)
    TEST_CHECK(!((r8 >> 29) & 1)); // CPUID.1:ECX.F16C (Phase 5)
    TEST_CHECK((r9 >> 5) & 1);     // CPUID.(7,0):EBX.AVX2
    TEST_MSG("cpuid.1.ecx=%08" PRIx64 " cpuid.7.ebx=%08" PRIx64, r8, r9);
    TEST_CHECK((rax & 0xffffffff) == 7 && (rdx & 0xffffffff) == 0); // XCR0 = x87|SSE|YMM
    TEST_MSG("xcr0=%08" PRIx64 ":%08" PRIx64, rdx, rax);

    OK(uc_close(uc));
}

// The VEX subset real msvcrt/ntdll/vcruntime140 memset/memcpy/strlen use (VEX.128 and VEX.256), mixed
// with one legacy SSE op. Expected values captured from native AVX2 hardware (Intel i7-14700).
static void test_x86_avx_golden(void)
{
    uc_engine *uc;
    char code[] = "\xc5\xfe\x6f\x00"             // vmovdqu ymm0, [rax]
                  "\xc5\xfe\x6f\x48\x20"         // vmovdqu ymm1, [rax+0x20]
                  "\xc5\xfd\x74\xd1"             // vpcmpeqb ymm2, ymm0, ymm1
                  "\xc5\xfd\xd7\xca"             // vpmovmskb ecx, ymm2
                  "\xc5\xfd\x75\x58\x20"         // vpcmpeqw ymm3, ymm0, [rax+0x20]
                  "\xc5\xfd\xd7\xd3"             // vpmovmskb edx, ymm3
                  "\xc5\xf9\xef\xe1"             // vpxor xmm4, xmm0, xmm1 (VEX.128 zeroes 255:128)
                  "\xc5\xfd\xef\xe9"             // vpxor ymm5, ymm0, ymm1
                  "\xc5\xf8\x10\x70\x40"         // vmovups xmm6, [rax+0x40]
                  "\xc4\xe3\x4d\x18\x70\x50\x01" // vinsertf128 ymm6, ymm6, [rax+0x50], 1
                  "\xc5\xfc\x28\x78\x40"         // vmovaps ymm7, [rax+0x40]
                  "\x66\x0f\xef\xff"             // pxor xmm7, xmm7 (legacy SSE keeps 255:128)
                  "\xc5\xfc\x11\x2b"             // vmovups [rbx], ymm5
                  "\xc5\xfc\x29\x73\x20"         // vmovaps [rbx+0x20], ymm6
                  "\xc5\xfd\x7f\x53\x40"         // vmovdqa [rbx+0x40], ymm2
                  "\xc5\xfd\xe7\x43\x60";        // vmovntdq [rbx+0x60], ymm0
    static const uint64_t expect_ymm[8][4] = {
        {0x342d261f18110a03ULL, 0x6c655e575049423bULL, 0xa49d968f88817a73ULL, 0xdcd5cec7c0b9b2abULL},
        {0x6e2d7c45184b0a03ULL, 0x6c3f04570a134261ULL, 0xfec796d5d2812029ULL, 0x86d5949dc0e3e8abULL},
        {0x00ff0000ff00ffffULL, 0xff0000ff0000ff00ULL, 0x0000ff0000ff0000ULL, 0x00ff0000ff0000ffULL},
        {0x000000000000ffffULL, 0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL},
        {0x5a005a5a005a0000ULL, 0x005a5a005a5a005aULL, 0x0000000000000000ULL, 0x0000000000000000ULL},
        {0x5a005a5a005a0000ULL, 0x005a5a005a5a005aULL, 0x5a5a005a5a005a5aULL, 0x5a005a5a005a5a00ULL},
        {0xc5b29f8c79665340ULL, 0x5d4a372411feebd8ULL, 0xf5e2cfbca9968370ULL, 0x8d7a6754412e1b08ULL},
        {0x0000000000000000ULL, 0x0000000000000000ULL, 0xf5e2cfbca9968370ULL, 0x8d7a6754412e1b08ULL},
    };
    static const uint64_t expect_mem[16] = {
        0x5a005a5a005a0000ULL, 0x005a5a005a5a005aULL, 0x5a5a005a5a005a5aULL, 0x5a005a5a005a5a00ULL,
        0xc5b29f8c79665340ULL, 0x5d4a372411feebd8ULL, 0xf5e2cfbca9968370ULL, 0x8d7a6754412e1b08ULL,
        0x00ff0000ff00ffffULL, 0xff0000ff0000ff00ULL, 0x0000ff0000ff0000ULL, 0x00ff0000ff0000ffULL,
        0x342d261f18110a03ULL, 0x6c655e575049423bULL, 0xa49d968f88817a73ULL, 0xdcd5cec7c0b9b2abULL,
    };
    uint8_t data[0x100];
    uint64_t rax = 0x3000, rbx = 0x3080, rcx, rdx;
    uint64_t ymm[4], mem[16];
    int i;

    for (i = 0; i < 0x20; i++) {
        data[i] = (uint8_t)(i * 7 + 3);                                 // A
        data[0x20 + i] = (i % 3 == 0) ? data[i] : (uint8_t)(data[i] ^ 0x5a); // B: equal to A where i % 3 == 0
        data[0x40 + i] = (uint8_t)(0x40 + i * 0x13);                    // C
    }
    data[0x21] = data[0x01]; // make word 0 fully equal
    memset(data + 0x60, 0, sizeof(data) - 0x60);

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);
    OK(uc_mem_write(uc, rax, data, sizeof(data)));
    OK(uc_reg_write(uc, UC_X86_REG_RAX, &rax));
    OK(uc_reg_write(uc, UC_X86_REG_RBX, &rbx));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    for (i = 0; i < 8; i++) {
        OK(uc_reg_read(uc, UC_X86_REG_YMM0 + i, ymm));
        TEST_CHECK(memcmp(ymm, expect_ymm[i], sizeof(ymm)) == 0);
        TEST_MSG("ymm%d = %016" PRIx64 ":%016" PRIx64 ":%016" PRIx64 ":%016" PRIx64, i, ymm[3], ymm[2],
                 ymm[1], ymm[0]);
    }
    OK(uc_reg_read(uc, UC_X86_REG_RCX, &rcx));
    OK(uc_reg_read(uc, UC_X86_REG_RDX, &rdx));
    TEST_CHECK(rcx == 0x4924924bULL);
    TEST_CHECK(rdx == 0x00000003ULL);
    TEST_MSG("rcx=%" PRIx64 " rdx=%" PRIx64, rcx, rdx);
    OK(uc_mem_read(uc, rbx, mem, sizeof(mem)));
    for (i = 0; i < 16; i++) {
        TEST_CHECK(mem[i] == expect_mem[i]);
        TEST_MSG("mem[%d] = %016" PRIx64, i, mem[i]);
    }

    OK(uc_close(uc));
}

// VZEROUPPER keeps bits 127:0 and clears 255:128; VZEROALL clears everything.
static void test_x86_avx_vzeroupper_vzeroall(void)
{
    uc_engine *uc;
    char code[] = "\xc5\xf8\x77"          // vzeroupper
                  "\xc5\xfc\x11\x00"      // vmovups [rax], ymm0
                  "\xc5\xfc\x77";         // vzeroall
    uint64_t in[4], out[4], mem[4], rax = 0x3000;
    int i;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);
    for (i = 0; i < 8; i++) {
        in[0] = 0x1111111111111111ULL * (i + 1);
        in[1] = in[0] + 1;
        in[2] = in[0] + 2;
        in[3] = in[0] + 3;
        OK(uc_reg_write(uc, UC_X86_REG_YMM0 + i, in));
    }
    OK(uc_reg_write(uc, UC_X86_REG_RAX, &rax));
    OK(uc_emu_start(uc, code_start, code_start + 7, 0, 0)); // vzeroupper + store
    for (i = 0; i < 8; i++) {
        OK(uc_reg_read(uc, UC_X86_REG_YMM0 + i, out));
        TEST_CHECK(out[0] == 0x1111111111111111ULL * (i + 1) && out[1] == out[0] + 1 && out[2] == 0 &&
                   out[3] == 0);
        TEST_MSG("after vzeroupper ymm%d = %016" PRIx64 ":%016" PRIx64 ":%016" PRIx64 ":%016" PRIx64, i,
                 out[3], out[2], out[1], out[0]);
    }
    OK(uc_mem_read(uc, rax, mem, sizeof(mem)));
    TEST_CHECK(mem[0] == 0x1111111111111111ULL && mem[1] == 0x1111111111111112ULL && mem[2] == 0 &&
               mem[3] == 0);
    OK(uc_emu_start(uc, code_start + 7, code_start + sizeof(code) - 1, 0, 0)); // vzeroall
    for (i = 0; i < 8; i++) {
        OK(uc_reg_read(uc, UC_X86_REG_YMM0 + i, out));
        TEST_CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0);
        TEST_MSG("after vzeroall ymm%d = %016" PRIx64 ":%016" PRIx64, i, out[1], out[0]);
    }

    OK(uc_close(uc));
}

// The original Unicorn repro: vinsertf128 ymm0, ymm0, xmm0, 1 (register form, as in msvcrt memset).
static void test_x86_avx_vinsertf128_repro(void)
{
    uc_engine *uc;
    char code[] = "\xc4\xe3\x7d\x18\xc0\x01"; // vinsertf128 ymm0, ymm0, xmm0, 1
    uint64_t in[4] = {0x0123456789abcdefULL, 0xfedcba9876543210ULL, 0x5555555555555555ULL,
                      0x6666666666666666ULL};
    uint64_t out[4];

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);
    OK(uc_reg_write(uc, UC_X86_REG_YMM0, in));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_YMM0, out));
    TEST_CHECK(out[0] == in[0] && out[1] == in[1] && out[2] == in[0] && out[3] == in[1]);
    TEST_MSG("ymm0 = %016" PRIx64 ":%016" PRIx64 ":%016" PRIx64 ":%016" PRIx64, out[3], out[2], out[1],
             out[0]);

    OK(uc_close(uc));
}

// BZHI with index >= operand size must return the source unchanged and set CF (upstream 9ad2ba6e8e).
// Expected values captured from native hardware.
static void test_x86_bzhi_index_ge_size(void)
{
    uc_engine *uc;
    char code[] = "\xc4\xe2\xf0\xf5\xc3" // bzhi rax, rbx, rcx
                  "\x9c\x41\x58"         // pushfq ; pop r8
                  "\xc4\xe2\xc8\xf5\xfb" // bzhi rdi, rbx, rsi
                  "\x9c\x41\x59";        // pushfq ; pop r9
    uint64_t rbx = 0x8123456789abcdefULL, rcx = 0x40, rsi = 0x3f, rsp = 0x3000, rax, rdi, r8, r9;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);
    OK(uc_reg_write(uc, UC_X86_REG_RBX, &rbx));
    OK(uc_reg_write(uc, UC_X86_REG_RCX, &rcx));
    OK(uc_reg_write(uc, UC_X86_REG_RSI, &rsi));
    OK(uc_reg_write(uc, UC_X86_REG_RSP, &rsp));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_RAX, &rax));
    OK(uc_reg_read(uc, UC_X86_REG_RDI, &rdi));
    OK(uc_reg_read(uc, UC_X86_REG_R8, &r8));
    OK(uc_reg_read(uc, UC_X86_REG_R9, &r9));

    TEST_CHECK(rax == 0x8123456789abcdefULL && (r8 & 0x8c1) == 0x81); // SF, CF
    TEST_MSG("rax=%016" PRIx64 " flags=%" PRIx64, rax, r8 & 0x8c1);
    TEST_CHECK(rdi == 0x0123456789abcdefULL && (r9 & 0x8c1) == 0);
    TEST_MSG("rdi=%016" PRIx64 " flags=%" PRIx64, rdi, r9 & 0x8c1);

    OK(uc_close(uc));
}

// Invalid or unsupported VEX forms must raise #UD, never abort the host through a decoder assertion.
// vpinsrw with VEX.pp=00 (an MMX form, #UD on hardware; used to hit assert(vec_len == 16)),
// VEX map 1 opcode 0x38 (#UD on hardware), and VEX.L=1 vroundss/vmovss (hardware ignores VEX.L;
// Unicorn rejects it like upstream does for other scalar ops, instead of asserting).
static void test_x86_vex_invalid_forms_no_abort(void)
{
    static const char *const codes[] = {
        "\xc4\xe1\x7c\xc4\xc1\x31", // vpinsrw xmm0, ymm0, ecx, 0x31 with VEX.pp=00, VEX.L=1
        "\xc4\xe1\x79\x38\xc1",     // VEX map 1, opcode 0x38
        "\xc4\xe3\x7d\x0a\xc1\x31", // vroundss xmm0, xmm0, xmm1, 0x31 with VEX.L=1
        "\xc4\xe1\x7e\x10\xc1",     // vmovss xmm0, xmm0, xmm1 with VEX.L=1
    };
    static const int lens[] = {6, 5, 6, 5};
    int i;

    for (i = 0; i < 4; i++) {
        uc_engine *uc;
        uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, codes[i], lens[i]);
        uc_assert_err(UC_ERR_INSN_INVALID, uc_emu_start(uc, code_start, code_start + lens[i], 0, 0));
        TEST_MSG("case %d", i);
        OK(uc_close(uc));
    }
}
```

#### `vexsweep.c` — md5 `1f5618b930e8431c2c71077ef2b052cc` (optional differential tool; build: `gcc -O1 -I<uc>/include vexsweep.c vexsweep_stub.S -L<uc>/build -lunicorn`)

```c
/* Differential VEX sweep: native AVX2 host vs Unicorn. Prints one line per mismatch. */
#define _GNU_SOURCE
#include <unicorn/unicorn.h>
#include <signal.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static double tw, te, tr;

typedef struct { uint8_t ymm[16][32]; uint32_t mxcsr, pad; uint64_t rax, rcx, rdx, rbx, rflags, rsi, rdi; } State;
void run_stub(State *st, void *code);

#define BUF 0x10000000ULL
#define BUFSZ 0x2000
#define CODE 0x20000000ULL
static sigjmp_buf jb;
static void onsig(int s) { siglongjmp(jb, s); }

static uint64_t rng = 88172645463325252ULL;
static uint64_t xr(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static void init_state(State *st, uint8_t *buf, uint64_t seed)
{
    int i, j;
    rng = seed * 2654435761ULL + 1;
    for (i = 0; i < 16; i++)
        for (j = 0; j < 32; j += 4) {
            uint32_t v;
            switch (xr() % 4) {
            case 0: { float f = (float)((int)(xr() % 2000) - 1000) / 8.0f; memcpy(&v, &f, 4); break; }
            case 1: v = (uint32_t)xr(); break;
            case 2: v = (uint32_t)(xr() % 256) * 0x01010101u; break;
            default: { double d = (double)((int)(xr() % 2000) - 1000) / 16.0; uint64_t q; memcpy(&q, &d, 8); v = (j & 4) ? (uint32_t)(q >> 32) : (uint32_t)q; }
            }
            memcpy(&st->ymm[i][j], &v, 4);
        }
    for (i = 0; i < BUFSZ; i++) buf[i] = (uint8_t)xr();
    st->mxcsr = 0x1f80;
    st->rax = BUF + 0x40;
    st->rcx = 0x0000000100000123ULL ^ (xr() & 0xffff0000ffffULL);
    st->rdx = xr();
    st->rbx = xr();
    st->rflags = 0x202;
    st->rsi = BUF + 0x100;
    st->rdi = BUF + 0x180;
}

static int insn_len(int map, int op, int modrm, uint8_t *imm)
{
    int has_imm = (map == 3) || (map == 1 && ((op >= 0x70 && op <= 0x73) || op == 0xc2 || (op >= 0xc4 && op <= 0xc6)));
    *imm = has_imm;
    return 3 + 1 + 1 + has_imm;
}

int main(int argc, char **argv)
{
    int only_map = argc > 1 ? atoi(argv[1]) : 0;
    int op_lo = argc > 2 ? strtol(argv[2], 0, 16) : 0, op_hi = argc > 3 ? strtol(argv[3], 0, 16) : 255;
    uint8_t *nbuf = mmap((void *)BUF, BUFSZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    uint8_t *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uc_engine *uc;
    uint64_t caseno = 0, n_mis = 0, n_ok = 0;
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = onsig; sa.sa_flags = SA_NODEFER;
    sigaction(SIGILL, &sa, 0); sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0); sigaction(SIGFPE, &sa, 0);
    if (nbuf != (void *)BUF) { perror("mmap buf"); return 1; }
    if (uc_open(UC_ARCH_X86, UC_MODE_64, &uc)) return 1;
    uc_mem_map(uc, BUF, BUFSZ, UC_PROT_ALL);
    uc_mem_map(uc, CODE, 0x1000000, UC_PROT_ALL);
    for (int map = 1; map <= 3; map++) {
        if (only_map && map != only_map) continue;
        for (int op = op_lo; op <= op_hi; op++)
        for (int pp = 0; pp < 4; pp++)
        for (int L = 0; L < 2; L++)
        for (int W = 0; W < 2; W++)
        for (int mi = 0; mi < 2; mi++)
        for (int vi = 0; vi < 2; vi++) {
            uint8_t code[16], hasimm; int len, k;
            int modrm = mi ? 0x00 : 0xc1;         /* [rax] or reg=0, rm=1 */
            int vvvv = vi ? 0xe : 0xf;            /* inverted: reg 1 or reg 0 */
            code[0] = 0xc4;
            code[1] = 0xe0 | map;                 /* R X B = 1 (not extended) */
            code[2] = (W << 7) | (vvvv << 3) | (L << 2) | pp;
            code[3] = op; code[4] = modrm;
            len = insn_len(map, op, modrm, &hasimm);
            if (hasimm) code[5] = 0x31;
            State s0, sn, su; uint8_t b0[BUFSZ], bu[BUFSZ];
            caseno++;
            init_state(&s0, b0, caseno);
            /* native */
            sn = s0; memcpy(nbuf, b0, BUFSZ);
            memcpy(page, code, len); page[len] = 0xc3;
            int nsig = sigsetjmp(jb, 1);
            if (!nsig) run_stub(&sn, page);
            /* unicorn */
            su = s0;
            uint64_t addr = CODE + (caseno % 0x100000) * 16;
            double t0 = now();
            uc_mem_write(uc, BUF, b0, BUFSZ);
            uc_mem_write(uc, addr, code, len);
            for (k = 0; k < 16; k++) uc_reg_write(uc, UC_X86_REG_YMM0 + k, su.ymm[k]);
            uc_reg_write(uc, UC_X86_REG_MXCSR, &su.mxcsr);
            uc_reg_write(uc, UC_X86_REG_RAX, &su.rax); uc_reg_write(uc, UC_X86_REG_RCX, &su.rcx);
            uc_reg_write(uc, UC_X86_REG_RDX, &su.rdx); uc_reg_write(uc, UC_X86_REG_RBX, &su.rbx);
            uc_reg_write(uc, UC_X86_REG_EFLAGS, &su.rflags);
            uc_reg_write(uc, UC_X86_REG_RSI, &su.rsi); uc_reg_write(uc, UC_X86_REG_RDI, &su.rdi);
            double t1 = now();
            uc_err e = uc_emu_start(uc, addr, addr + len, 0, 0);
            double t2 = now(); tw += t1 - t0; te += t2 - t1;
            const char *nv = nsig == SIGILL ? "UD" : nsig ? "FAULT" : "OK";
            const char *uv = e == UC_ERR_INSN_INVALID ? "UD" : e ? "FAULT" : "OK";
            char hex[40]; for (k = 0; k < len; k++) sprintf(hex + 2 * k, "%02x", code[k]);
            if (strcmp(nv, uv)) { printf("VALID %s native=%s uc=%s(%d) map=%d op=%02x pp=%d L=%d W=%d mod=%s v=%d\n", hex, nv, uv, e, map, op, pp, L, W, mi ? "mem" : "reg", vi); n_mis++; continue; }
            if (strcmp(nv, "OK")) continue;
            n_ok++;
            for (k = 0; k < 16; k++) uc_reg_read(uc, UC_X86_REG_YMM0 + k, su.ymm[k]);
            uc_reg_read(uc, UC_X86_REG_MXCSR, &su.mxcsr);
            uc_reg_read(uc, UC_X86_REG_RAX, &su.rax); uc_reg_read(uc, UC_X86_REG_RCX, &su.rcx);
            uc_reg_read(uc, UC_X86_REG_RDX, &su.rdx); uc_reg_read(uc, UC_X86_REG_RBX, &su.rbx);
            uc_reg_read(uc, UC_X86_REG_EFLAGS, &su.rflags);
            uc_reg_read(uc, UC_X86_REG_RSI, &su.rsi); uc_reg_read(uc, UC_X86_REG_RDI, &su.rdi);
            uc_mem_read(uc, BUF, bu, BUFSZ);
            char what[256] = "";
            for (k = 0; k < 16; k++) if (memcmp(sn.ymm[k], su.ymm[k], 32)) { char t[16]; sprintf(t, " ymm%d", k); strcat(what, t); }
            if (sn.rax != su.rax) { char t[48]; sprintf(t, " rax(%llx/%llx)", (unsigned long long)sn.rax, (unsigned long long)su.rax); strcat(what, t); }
            if (sn.rcx != su.rcx) strcat(what, " rcx");
            if (sn.rdx != su.rdx) strcat(what, " rdx");
            if (sn.rbx != su.rbx) strcat(what, " rbx");
            if (sn.rsi != su.rsi) strcat(what, " rsi");
            if (sn.rdi != su.rdi) strcat(what, " rdi");
            if ((sn.rflags ^ su.rflags) & 0x8d5) { char t[40]; sprintf(t, " flags(%llx/%llx)", (unsigned long long)(sn.rflags & 0x8d5), (unsigned long long)(su.rflags & 0x8d5)); strcat(what, t); }
            if (sn.mxcsr != su.mxcsr) { char t[40]; sprintf(t, " mxcsr(%x/%x)", sn.mxcsr, su.mxcsr); strcat(what, t); }
            if (memcmp(nbuf, bu, BUFSZ)) strcat(what, " mem");
            if (what[0]) { printf("VALUE %s map=%d op=%02x pp=%d L=%d W=%d mod=%s v=%d:%s\n", hex, map, op, pp, L, W, mi ? "mem" : "reg", vi, what); n_mis++; }
        }
    }
    fprintf(stderr, "t_setup=%.2fs t_emu=%.2fs\n", tw, te);
    fprintf(stderr, "cases=%llu both-ok=%llu mismatches=%llu\n", (unsigned long long)caseno, (unsigned long long)n_ok, (unsigned long long)n_mis);
    return 0;
}
```

#### `vexsweep_stub.S` — md5 `60a92506359c37778c0cdc00d9c3d4a9` (optional differential tool; build: `gcc -O1 -I<uc>/include vexsweep.c vexsweep_stub.S -L<uc>/build -lunicorn`)

```asm
    .intel_syntax noprefix
    .text
    .globl run_stub
/* void run_stub(State *st, void *code): st layout: ymm[16][32] @0, mxcsr @0x200, rax @0x208, rcx @0x210, rdx @0x218, rbx @0x220, rflags @0x228 */
run_stub:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov r15, rdi
    mov r14, rsi
    .irp i,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    vmovdqu ymm\i, [r15 + 32*\i]
    .endr
    ldmxcsr [r15 + 0x200]
    mov rax, [r15 + 0x208]
    mov rcx, [r15 + 0x210]
    mov rdx, [r15 + 0x218]
    mov rbx, [r15 + 0x220]
    mov rsi, [r15 + 0x230]
    mov rdi, [r15 + 0x238]
    push qword ptr [r15 + 0x228]
    popfq
    call r14
    pushfq
    pop qword ptr [r15 + 0x228]
    mov [r15 + 0x208], rax
    mov [r15 + 0x210], rcx
    mov [r15 + 0x218], rdx
    mov [r15 + 0x220], rbx
    mov [r15 + 0x230], rsi
    mov [r15 + 0x238], rdi
    stmxcsr [r15 + 0x200]
    .irp i,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    vmovdqu [r15 + 32*\i], ymm\i
    .endr
    vzeroupper
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret
    .section .note.GNU-stack,"",@progbits
```
