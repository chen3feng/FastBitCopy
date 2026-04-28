// Copyright (c) chen3feng. All Rights Reserved.
//
// Minimal cross-platform runtime function hook.
//
//  x86_64: we patch the first 5 bytes of the target with `E9 rel32` (near
//          jump). If the detour is further than +/- 2GiB away, we fall back
//          to a 14-byte absolute jump `FF 25 00 00 00 00 | imm64`. The bytes
//          displaced are copied into a page-aligned executable trampoline,
//          followed by a 14-byte absolute jump to `Target + PatchSize` so the
//          original function remains callable.
//
//  aarch64: we patch 16 bytes of the target with
//                LDR  X16, #8
//                BR   X16
//                <imm64 = Detour>
//          The 4 displaced instructions (16 bytes) are moved into the
//          trampoline, followed by an identical LDR/BR absolute jump back
//          into the middle of the original function. Only straight-line code
//          is supported; if the first 16 bytes contain a PC-relative branch
//          / ADR / ADRP the hook will refuse to install (extremely unlikely
//          for `appBitsCpy`, but we check anyway).
//
// Code organization
// -----------------
// To keep individual functions free of `#if PLATFORM_CPU_*`, the file is
// organized in top-level architecture-specific blocks:
//
//   * Shared OS helpers      (AllocExecutable, FScopedUnprotect, ...)
//   * x86_64 block           (InstrLen, Relocate, InstallHook_X64)
//   * aarch64 block          (IsRelocatableInsn, InstallHook_Arm64)
//   * Common dispatch entry  (FFunctionHook::Install / Uninstall)
//
// Each per-arch `InstallHook_*` returns a small `FInstallResult` value; the
// dispatcher copies its fields into the owning `FFunctionHook` instance.

#include "FunctionHook.h"

#include "HAL/PlatformMisc.h"
#include "HAL/UnrealMemory.h"
#include "Misc/AssertionMacros.h"

#if PLATFORM_WINDOWS
	#include "Windows/AllowWindowsPlatformTypes.h"
	#include <windows.h>
	#include "Windows/HideWindowsPlatformTypes.h"
#else
	#include <sys/mman.h>
	#include <unistd.h>
	#include <errno.h>
	#include <string.h>
#endif

#if PLATFORM_MAC
	#include <libkern/OSCacheControl.h>
	#include <pthread.h>
#endif

// ----------------------------------------------------------------------------
// Shared OS helpers: allocate RWX memory, change page permissions, flush icache
// (architecture-agnostic)
// ----------------------------------------------------------------------------

namespace FastBitCopyHookPrivate
{
	/** Result produced by an arch-specific installer; copied back into the owning FFunctionHook. */
	struct FInstallResult
	{
		void *TargetAddr = nullptr;
		void *TrampolineMem = nullptr;
		uint8 OriginalBytes[32] = {};
		int32 PatchSize = 0;
		bool bOk = false;
	};

	static SIZE_T GetPageSize()
	{
#if PLATFORM_WINDOWS
		SYSTEM_INFO SysInfo;
		GetSystemInfo(&SysInfo);
		return (SIZE_T)SysInfo.dwPageSize;
#else
		long PS = sysconf(_SC_PAGESIZE);
		return PS > 0 ? (SIZE_T)PS : 4096;
#endif
	}

	// Allocate one page of RWX memory, ideally within +/- 2GiB of `NearAddr`
	// on x86_64 so we can reach the detour with a 5-byte rel32 jump.
	static void* AllocExecutable(void* NearAddr, SIZE_T Size)
	{
		const SIZE_T PageSize = GetPageSize();
		Size = ((Size + PageSize - 1) / PageSize) * PageSize;

#if PLATFORM_WINDOWS
		// Try to allocate near NearAddr so a rel32 jump is enough.
		if (NearAddr)
		{
			const uintptr_t Base = (uintptr_t)NearAddr;
			const uintptr_t Range = (uintptr_t)0x20000000; // +/- 512MB window
			for (uintptr_t Delta = PageSize; Delta < Range; Delta += PageSize * 64)
			{
				void* Candidates[2] = {
					(void*)(Base + Delta),
					(void*)(Base - Delta),
				};
				for (void* C : Candidates)
				{
					void* P = VirtualAlloc(C, Size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
					if (P) return P;
				}
			}
		}
		return VirtualAlloc(nullptr, Size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
#else
		int Prot = PROT_READ | PROT_WRITE | PROT_EXEC;
		int Flags = MAP_PRIVATE | MAP_ANONYMOUS;

	#if PLATFORM_MAC && PLATFORM_CPU_ARM_FAMILY
		// On Apple Silicon W^X is enforced; request a JIT page.
		Flags |= MAP_JIT;
		Prot = PROT_READ | PROT_EXEC; // will switch to write via pthread_jit_write_protect_np
	#endif

		void* Hint = nullptr;
		(void)NearAddr; // we do not currently bias allocations on POSIX
		void* P = mmap(Hint, Size, Prot, Flags, -1, 0);
		if (P == MAP_FAILED) return nullptr;
		return P;
#endif
	}

	static void FreeExecutable(void* Addr, SIZE_T Size)
	{
		if (!Addr) return;
#if PLATFORM_WINDOWS
		(void)Size;
		VirtualFree(Addr, 0, MEM_RELEASE);
#else
		const SIZE_T PageSize = GetPageSize();
		Size = ((Size + PageSize - 1) / PageSize) * PageSize;
		munmap(Addr, Size);
#endif
	}

	// Temporarily make the code pages at [Addr, Addr+Size) writable.
	// On Apple Silicon we also toggle JIT write protect for the thread.
	// `bIsJitPage` must be true when unprotecting a MAP_JIT region (i.e.
	// the trampoline we allocated ourselves on mac-arm64). Regular loaded
	// code pages (libUnrealEditor-Core.dylib / .so / .dll) should use
	// bIsJitPage = false; on mac-arm64 the kernel allows writing those
	// via mprotect just like on other OSes.
	class FScopedUnprotect
	{
	public:
		FScopedUnprotect(void* InAddr, SIZE_T InSize, bool bInIsJitPage = false)
			: Addr(InAddr), Size(InSize), bIsJitPage(bInIsJitPage)
		{
#if PLATFORM_WINDOWS
			(void)bIsJitPage;
			VirtualProtect(Addr, Size, PAGE_EXECUTE_READWRITE, (DWORD*)&OldProt);
#else
			const SIZE_T PageSize = GetPageSize();
			uintptr_t Start = (uintptr_t)Addr & ~(PageSize - 1);
			uintptr_t End = ((uintptr_t)Addr + Size + PageSize - 1) & ~(PageSize - 1);
			PageAddr = (void*)Start;
			PageSpan = (SIZE_T)(End - Start);

	#if PLATFORM_MAC && PLATFORM_CPU_ARM_FAMILY
			if (bIsJitPage)
			{
				// MAP_JIT page: toggle write protect instead of mprotect.
				pthread_jit_write_protect_np(0);
			}
			else
			{
				mprotect(PageAddr, PageSpan, PROT_READ | PROT_WRITE | PROT_EXEC);
			}
	#else
			mprotect(PageAddr, PageSpan, PROT_READ | PROT_WRITE | PROT_EXEC);
	#endif
#endif
		}
		~FScopedUnprotect()
		{
#if PLATFORM_WINDOWS
			DWORD Tmp = 0;
			VirtualProtect(Addr, Size, (DWORD)OldProt, &Tmp);
			FlushInstructionCache(GetCurrentProcess(), Addr, Size);
#else
	#if PLATFORM_MAC && PLATFORM_CPU_ARM_FAMILY
			if (bIsJitPage)
			{
				pthread_jit_write_protect_np(1); // back to executable
				sys_icache_invalidate(Addr, Size);
			}
			else
			{
				mprotect(PageAddr, PageSpan, PROT_READ | PROT_EXEC);
				sys_icache_invalidate(Addr, Size);
			}
	#else
			mprotect(PageAddr, PageSpan, PROT_READ | PROT_EXEC);
		#if PLATFORM_CPU_ARM_FAMILY
			__builtin___clear_cache((char*)Addr, (char*)Addr + Size);
		#endif
	#endif
#endif
		}
	private:
		void*  Addr = nullptr;
		SIZE_T Size = 0;
		bool   bIsJitPage = false;
#if PLATFORM_WINDOWS
		DWORD  OldProt = 0;
#else
		void*  PageAddr = nullptr;
		SIZE_T PageSpan = 0;
#endif
	};

	static void FlushIcache(void* Addr, SIZE_T Size)
	{
#if PLATFORM_WINDOWS
		FlushInstructionCache(GetCurrentProcess(), Addr, Size);
#elif PLATFORM_MAC
		sys_icache_invalidate(Addr, Size);
#else
		__builtin___clear_cache((char*)Addr, (char*)Addr + Size);
#endif
	}
} // namespace FastBitCopyHookPrivate

// ============================================================================
// x86_64 block
// ============================================================================
#if PLATFORM_CPU_X86_FAMILY
namespace FastBitCopyHookPrivate
{
	// Write a 5-byte `E9 rel32` jmp at `At` targeting `To`.
	static void WriteJmpRel32(uint8 *At, void *To)
	{
		int64 Rel = (int64)((intptr_t)To - (intptr_t)At - 5);
		At[0] = 0xE9;
		*(int32 *)(At + 1) = (int32)Rel;
	}

	// Write a 14-byte absolute `FF 25 00 00 00 00 | imm64` jmp at `At` targeting `To`.
	static void WriteJmpAbs14(uint8 *At, void *To)
	{
		At[0] = 0xFF;
		At[1] = 0x25;
		At[2] = 0x00;
		At[3] = 0x00;
		At[4] = 0x00;
		At[5] = 0x00;
		*(uint64 *)(At + 6) = (uint64)(uintptr_t)To;
	}

	// Extremely small length-disassembler: just enough to figure out how many
	// bytes the first "at least N" instructions of a function occupy.
	// Handles the instruction families typically emitted as prologue:
	//   push/pop reg, mov reg,reg, sub/add reg,imm, lea, endbr64, nop,
	//   rex-prefix + above, 0F 1F .. (multi-byte NOPs), cld, etc.
	// If it sees something it doesn't understand it returns 0.
	static int InstrLen(const uint8 *P)
	{
		int Len = 0;
		bool bHasOperandSize = false;
		bool bHasAddressSize = false;
		// prefixes
		while (true)
		{
			uint8 b = P[Len];
			if (b == 0x66)
			{
				bHasOperandSize = true;
				++Len;
				continue;
			}
			if (b == 0x67)
			{
				bHasAddressSize = true;
				++Len;
				continue;
			}
			if (b == 0xF0 || b == 0xF2 || b == 0xF3)
			{
				++Len;
				continue;
			}
			if (b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65)
			{
				++Len;
				continue;
			}
			break;
		}
		// REX
		bool bHasRex = false;
		if ((P[Len] & 0xF0) == 0x40)
		{
			bHasRex = true;
			++Len;
		}

		uint8 Op = P[Len++];

		auto ModRMSize = [&](uint8 Mod, uint8 RM) -> int
		{
			int S = 1; // modrm itself
			if (Mod == 3)
				return S;
			bool bHasSIB = (RM == 4);
			if (bHasSIB)
				S += 1;
			if (Mod == 1)
				S += 1;
			else if (Mod == 2)
				S += 4;
			else if (Mod == 0 && RM == 5)
				S += 4; // rip-relative or disp32
			else if (Mod == 0 && bHasSIB)
			{
				uint8 SIB = P[Len];
				uint8 Base = SIB & 7;
				if (Base == 5)
					S += 4;
			}
			return S;
		};

		auto DecodeModRM = [&](int ImmSize)
		{
			uint8 MR = P[Len];
			uint8 Mod = (MR >> 6) & 3;
			uint8 RM = MR & 7;
			Len += ModRMSize(Mod, RM);
			Len += ImmSize;
		};

		// Common opcodes
		switch (Op)
		{
		// 1-byte opcodes, no operand
		case 0x50:
		case 0x51:
		case 0x52:
		case 0x53:
		case 0x54:
		case 0x55:
		case 0x56:
		case 0x57: // push r64
		case 0x58:
		case 0x59:
		case 0x5A:
		case 0x5B:
		case 0x5C:
		case 0x5D:
		case 0x5E:
		case 0x5F: // pop r64
		case 0x90: // nop
		case 0x98: // cbw/cwde/cdqe
		case 0x99: // cwd/cdq/cqo
		case 0xC3: // ret
		case 0xFC: // cld
		case 0xFD: // std
			return Len;

		// endbr64 (F3 0F 1E FA) handled via F3 prefix above; here 0F 1E.
		case 0x0F:
		{
			uint8 Op2 = P[Len++];
			switch (Op2)
			{
			case 0x1F: // multi-byte nop with modrm
				DecodeModRM(0);
				return Len;
			case 0x1E: // endbr64 etc.: reg/mem w/ modrm
				DecodeModRM(0);
				return Len;
			// 0F B6 /r  movzx r32, r/m8
			// 0F B7 /r  movzx r32, r/m16
			// 0F BE /r  movsx r32, r/m8
			// 0F BF /r  movsx r32, r/m16
			case 0xB6:
			case 0xB7:
			case 0xBE:
			case 0xBF:
				DecodeModRM(0);
				return Len;
			// 0F 80..8F rel32  near conditional jumps (Jcc)
			case 0x80:
			case 0x81:
			case 0x82:
			case 0x83:
			case 0x84:
			case 0x85:
			case 0x86:
			case 0x87:
			case 0x88:
			case 0x89:
			case 0x8A:
			case 0x8B:
			case 0x8C:
			case 0x8D:
			case 0x8E:
			case 0x8F:
				Len += 4; // rel32
				return Len;
			// 0F 40..4F /r  CMOVcc r, r/m
			case 0x40:
			case 0x41:
			case 0x42:
			case 0x43:
			case 0x44:
			case 0x45:
			case 0x46:
			case 0x47:
			case 0x48:
			case 0x49:
			case 0x4A:
			case 0x4B:
			case 0x4C:
			case 0x4D:
			case 0x4E:
			case 0x4F:
				DecodeModRM(0);
				return Len;
			// 0F 90..9F /r  SETcc r/m8
			case 0x90:
			case 0x91:
			case 0x92:
			case 0x93:
			case 0x94:
			case 0x95:
			case 0x96:
			case 0x97:
			case 0x98:
			case 0x99:
			case 0x9A:
			case 0x9B:
			case 0x9C:
			case 0x9D:
			case 0x9E:
			case 0x9F:
				DecodeModRM(0);
				return Len;
			default:
				return 0; // unsupported
			}
		}

		// MOV reg, imm32 (B8+rd)
		case 0xB8:
		case 0xB9:
		case 0xBA:
		case 0xBB:
		case 0xBC:
		case 0xBD:
		case 0xBE:
		case 0xBF:
			Len += bHasRex ? 8 : (bHasOperandSize ? 2 : 4);
			return Len;

		// 83 /digit ib : ADD/SUB/AND/... r/m32, imm8
		case 0x83:
			DecodeModRM(1);
			return Len;

		// 81 /digit id
		case 0x81:
			DecodeModRM(bHasOperandSize ? 2 : 4);
			return Len;

		// 89 /r  mov r/m, r
		// 8B /r  mov r, r/m
		// 8D /r  lea
		// 85 /r  test
		// 31 /r  xor
		// 01 /r  add
		// 29 /r  sub
		// 39 /r  cmp r/m, r
		// 3B /r  cmp r, r/m
		// 3A /r  cmp r8, r/m8
		// 8A /r, 88 /r  mov byte
		// 09 /r  or r/m, r
		// 0B /r  or r, r/m
		// 21 /r  and r/m, r
		// 23 /r  and r, r/m
		case 0x88:
		case 0x89:
		case 0x8A:
		case 0x8B:
		case 0x8D:
		case 0x85:
		case 0x31:
		case 0x01:
		case 0x29:
		case 0x39:
		case 0x84:
		case 0x30:
		case 0x00:
		case 0x28:
		case 0x38:
		case 0x3A:
		case 0x3B:
		case 0x09:
		case 0x0B:
		case 0x21:
		case 0x23:
			DecodeModRM(0);
			return Len;

		// Short conditional jumps: 70..7F rel8 (Jcc short)
		case 0x70:
		case 0x71:
		case 0x72:
		case 0x73:
		case 0x74:
		case 0x75:
		case 0x76:
		case 0x77:
		case 0x78:
		case 0x79:
		case 0x7A:
		case 0x7B:
		case 0x7C:
		case 0x7D:
		case 0x7E:
		case 0x7F:
			Len += 1; // rel8
			return Len;

		// EB rel8  short unconditional jump
		case 0xEB:
			Len += 1; // rel8
			return Len;

		// E9 rel32  near unconditional jump
		case 0xE9:
			Len += 4; // rel32
			return Len;

		// E8 rel32  near call
		case 0xE8:
			Len += 4; // rel32
			return Len;

		// C7 /0 id  mov r/m32, imm32 (or r/m64 with REX.W)
		case 0xC7:
			DecodeModRM(bHasOperandSize ? 2 : 4);
			return Len;

		// C6 /0 ib  mov r/m8, imm8
		case 0xC6:
			DecodeModRM(1);
			return Len;

		// 80 /digit ib  ADD/OR/ADC/SBB/AND/SUB/XOR/CMP r/m8, imm8
		case 0x80:
			DecodeModRM(1);
			return Len;

		// FF /r  (inc/dec/call/jmp/push r/m)
		case 0xFF:
		{
			uint8 MR = P[Len];
			uint8 Reg = (MR >> 3) & 7;
			// /0 = inc, /1 = dec, /2 = call, /4 = jmp, /6 = push
			if (Reg == 0 || Reg == 1 || Reg == 2 || Reg == 4 || Reg == 6)
			{
				DecodeModRM(0);
				return Len;
			}
			return 0;
		}

		default:
			return 0; // unsupported — caller will bail out
		}
	}

	// After copying the displaced prologue bytes into the trampoline, any
	// PC-relative instruction (Jcc rel8/rel32, JMP rel8/rel32, CALL rel32,
	// RIP-relative ModRM) must have its offset adjusted because the
	// trampoline lives at a different address than the original code.
	//
	// We walk the copied bytes instruction-by-instruction and patch each
	// relative offset so it still points to the same absolute target.
	static void RelocateTrampoline(uint8 *Tramp, const uint8 *Orig, int32 PatchSize)
	{
		int32 Off = 0;
		while (Off < PatchSize)
		{
			uint8 *TP = Tramp + Off;
			const uint8 *OP = Orig + Off;
			int N = InstrLen(TP);
			if (N <= 0)
				break; // should not happen — already validated

			// Skip prefixes (same logic as InstrLen)
			int PrefixLen = 0;
			while (PrefixLen < N)
			{
				uint8 b = TP[PrefixLen];
				if (b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
					b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65)
				{
					++PrefixLen;
					continue;
				}
				break;
			}
			// REX prefix
			int OpOff = PrefixLen;
			if (OpOff < N && (TP[OpOff] & 0xF0) == 0x40)
				++OpOff;

			uint8 Op = TP[OpOff];
			const intptr_t Delta = (intptr_t)TP - (intptr_t)OP; // Tramp - Orig

			// Short conditional jump: 70..7F rel8
			if (Op >= 0x70 && Op <= 0x7F)
			{
				int8 Rel8 = (int8)TP[OpOff + 1];
				int32 NewRel = (int32)Rel8 - (int32)Delta;
				// If the new offset fits in int8, patch in place.
				// Otherwise we cannot fix it (would need to widen to 0F 8x rel32),
				// but for short-range jumps within the same function this is
				// almost always fine.
				if (NewRel >= -128 && NewRel <= 127)
				{
					TP[OpOff + 1] = (uint8)(int8)NewRel;
				}
				// else: leave as-is; the jump target is likely within the
				// trampoline itself or very close — best effort.
			}
			// Short unconditional jump: EB rel8
			else if (Op == 0xEB)
			{
				int8 Rel8 = (int8)TP[OpOff + 1];
				int32 NewRel = (int32)Rel8 - (int32)Delta;
				if (NewRel >= -128 && NewRel <= 127)
				{
					TP[OpOff + 1] = (uint8)(int8)NewRel;
				}
			}
			// Near unconditional jump: E9 rel32
			else if (Op == 0xE9)
			{
				int32 Rel32 = *(int32 *)(TP + OpOff + 1);
				int64 NewRel = (int64)Rel32 - (int64)Delta;
				if (NewRel >= INT32_MIN && NewRel <= INT32_MAX)
				{
					*(int32 *)(TP + OpOff + 1) = (int32)NewRel;
				}
			}
			// Near call: E8 rel32
			else if (Op == 0xE8)
			{
				int32 Rel32 = *(int32 *)(TP + OpOff + 1);
				int64 NewRel = (int64)Rel32 - (int64)Delta;
				if (NewRel >= INT32_MIN && NewRel <= INT32_MAX)
				{
					*(int32 *)(TP + OpOff + 1) = (int32)NewRel;
				}
			}
			// Two-byte near conditional jump: 0F 80..8F rel32
			else if (Op == 0x0F && (OpOff + 1) < N)
			{
				uint8 Op2 = TP[OpOff + 1];
				if (Op2 >= 0x80 && Op2 <= 0x8F)
				{
					int32 Rel32 = *(int32 *)(TP + OpOff + 2);
					int64 NewRel = (int64)Rel32 - (int64)Delta;
					if (NewRel >= INT32_MIN && NewRel <= INT32_MAX)
					{
						*(int32 *)(TP + OpOff + 2) = (int32)NewRel;
					}
				}
			}

			Off += N;
		}
	}

	// x86_64 installer. Writes the result into `Out`; returns Out.bOk.
	static FInstallResult InstallHook_X64(void *Target, void *Detour)
	{
		FInstallResult R;

		// Step 1: figure out how many bytes of prologue we need to relocate.
		uint8 *T = (uint8 *)Target;
		int32 Needed = 5; // E9 rel32
		const int32 MaxNeeded = 14;
		int32 Copied = 0;
		while (Copied < Needed)
		{
			int N = InstrLen(T + Copied);
			if (N <= 0)
			{
				// Can't decode; fall back to abs 14-byte patch which always works
				// if the prologue has at least 14 safe bytes — but we can't verify
				// without decoding, so fail safely.
				if (Needed == 5)
				{
					// try again demanding more so we can attempt absolute jump
					Needed = MaxNeeded;
					continue;
				}
				UE_LOG(LogTemp, Warning,
					   TEXT("FastBitCopy: InstrLen failed at offset %d, byte=0x%02X. "
							"Cannot decode prologue for hook installation."),
					   Copied, T[Copied]);
				return R;
			}
			Copied += N;
		}

		R.PatchSize = Copied;
		check(R.PatchSize <= (int32)sizeof(R.OriginalBytes));
		FMemory::Memcpy(R.OriginalBytes, T, R.PatchSize);

		// Step 2: allocate an executable trampoline (PatchSize + 14 bytes for abs jump back)
		SIZE_T TrampSize = (SIZE_T)R.PatchSize + 14;
		void *Tramp = AllocExecutable(Target, TrampSize);
		if (!Tramp)
			return R;
		FMemory::Memcpy(Tramp, R.OriginalBytes, R.PatchSize);
		RelocateTrampoline((uint8 *)Tramp, T, R.PatchSize);
		WriteJmpAbs14((uint8 *)Tramp + R.PatchSize, (uint8 *)Target + R.PatchSize);

		// Step 3: patch the prologue.
		{
			FScopedUnprotect Unprot(Target, (SIZE_T)R.PatchSize);
			int64 Rel = (int64)((intptr_t)Detour - (intptr_t)Target - 5);
			if (Needed == 5 && Rel >= INT32_MIN && Rel <= INT32_MAX)
			{
				WriteJmpRel32(T, Detour);
				// Pad the rest with NOPs so a debugger / profiler sees clean insns.
				for (int32 i = 5; i < R.PatchSize; ++i)
					T[i] = 0x90;
			}
			else
			{
				// Absolute 14-byte jump.
				if (R.PatchSize < 14)
				{
					// We need 14 bytes but only decoded fewer; extend by decoding more
					while (Copied < 14)
					{
						int N = InstrLen(T + Copied);
						if (N <= 0)
						{
							FreeExecutable(Tramp, TrampSize);
							return R;
						}
						Copied += N;
					}
					// Reallocate trampoline with new PatchSize
					FreeExecutable(Tramp, TrampSize);
					R.PatchSize = Copied;
					FMemory::Memcpy(R.OriginalBytes, T, R.PatchSize);
					TrampSize = (SIZE_T)R.PatchSize + 14;
					Tramp = AllocExecutable(Target, TrampSize);
					if (!Tramp)
						return R;
					FMemory::Memcpy(Tramp, R.OriginalBytes, R.PatchSize);
					RelocateTrampoline((uint8 *)Tramp, T, R.PatchSize);
					WriteJmpAbs14((uint8 *)Tramp + R.PatchSize, (uint8 *)Target + R.PatchSize);
				}
				WriteJmpAbs14(T, Detour);
				for (int32 i = 14; i < R.PatchSize; ++i)
					T[i] = 0x90;
			}
		}
		FlushIcache(Target, (SIZE_T)R.PatchSize);
		FlushIcache(Tramp, TrampSize);

		R.TargetAddr = Target;
		R.TrampolineMem = Tramp;
		R.bOk = true;
		return R;
	}
} // namespace FastBitCopyHookPrivate
#endif // PLATFORM_CPU_X86_FAMILY

// ============================================================================
// aarch64 block
// ============================================================================
#if PLATFORM_CPU_ARM_FAMILY
namespace FastBitCopyHookPrivate
{
	// Encode 16 bytes:
	//   LDR  X16, #8    ; 58000050
	//   BR   X16        ; D61F0200
	//   <imm64>
	static void WriteJmpAbs16(uint8 *At, void *To)
	{
		*(uint32 *)(At + 0) = 0x58000050u;
		*(uint32 *)(At + 4) = 0xD61F0200u;
		*(uint64 *)(At + 8) = (uint64)(uintptr_t)To;
	}

	// Reject instructions whose encoding is PC-relative and therefore would
	// break when relocated: B, BL, B.cond, CBZ/CBNZ, TBZ/TBNZ, ADR, ADRP,
	// and LDR (literal). We only need to validate the first 4 instructions.
	static bool IsRelocatableInsn(uint32 Insn)
	{
		// B       000101 imm26            -> top 6 bits 000101 (0x14000000..)
		// BL      100101 imm26            -> top 6 bits 100101 (0x94000000..)
		if ((Insn & 0x7C000000u) == 0x14000000u)
			return false; // B / BL
		// B.cond  01010100 .............. -> 0x54xxxxxx
		if ((Insn & 0xFF000010u) == 0x54000000u)
			return false;
		// CBZ/CBNZ  x0110100 / x0110101
		if ((Insn & 0x7E000000u) == 0x34000000u)
			return false;
		// TBZ/TBNZ  x0110110 / x0110111
		if ((Insn & 0x7E000000u) == 0x36000000u)
			return false;
		// ADR / ADRP  0xx10000 ...
		if ((Insn & 0x1F000000u) == 0x10000000u)
			return false;
		// LDR (literal)  0x011000 ...  (01011000 / 00011000 / 10011000 / ...)
		if ((Insn & 0x3B000000u) == 0x18000000u)
			return false;
		return true;
	}

	// aarch64 installer.
	static FInstallResult InstallHook_Arm64(void *Target, void *Detour)
	{
		FInstallResult R;

		// Validate and copy the first 16 bytes (4 instructions).
		const int32 kPatch = 16;
		uint32 *T32 = (uint32 *)Target;
		for (int32 i = 0; i < 4; ++i)
		{
			if (!IsRelocatableInsn(T32[i]))
			{
				return R;
			}
		}
		R.PatchSize = kPatch;
		FMemory::Memcpy(R.OriginalBytes, Target, kPatch);

		const SIZE_T TrampSize = (SIZE_T)kPatch + 16; // prologue + abs-16 jump back
		void *Tramp = AllocExecutable(Target, TrampSize);
		if (!Tramp)
			return R;

		{
			// Trampoline is already RWX (or we need to toggle on Apple Silicon via FScopedUnprotect
			// logic — AllocExecutable requested RX on mac-arm64). So unprotect to write.
			FScopedUnprotect Unprot(Tramp, TrampSize, /*bIsJitPage=*/true);
			FMemory::Memcpy(Tramp, R.OriginalBytes, kPatch);
			WriteJmpAbs16((uint8 *)Tramp + kPatch, (uint8 *)Target + kPatch);
		}
		FlushIcache(Tramp, TrampSize);

		{
			FScopedUnprotect Unprot(Target, (SIZE_T)kPatch);
			WriteJmpAbs16((uint8 *)Target, Detour);
		}
		FlushIcache(Target, (SIZE_T)kPatch);

		R.TargetAddr = Target;
		R.TrampolineMem = Tramp;
		R.bOk = true;
		return R;
	}
} // namespace FastBitCopyHookPrivate
#endif // PLATFORM_CPU_ARM_FAMILY

// ============================================================================
// Common dispatch entry points
// ============================================================================

using namespace FastBitCopyHookPrivate;

FFunctionHook::FFunctionHook() = default;
FFunctionHook::~FFunctionHook()
{
	if (bInstalled)
		Uninstall();
}

bool FFunctionHook::Install(void *Target, void *Detour, void **OutTrampoline)
{
	check(Target);
	check(Detour);
	if (bInstalled)
		return false;

#if PLATFORM_CPU_X86_FAMILY || PLATFORM_CPU_ARM_FAMILY
#if PLATFORM_CPU_X86_FAMILY
	const FInstallResult R = InstallHook_X64(Target, Detour);
#else
	const FInstallResult R = InstallHook_Arm64(Target, Detour);
#endif

	if (!R.bOk)
		return false;

	TargetAddr = R.TargetAddr;
	TrampolineMem = R.TrampolineMem;
	PatchSize = R.PatchSize;
	FMemory::Memcpy(OriginalBytes, R.OriginalBytes, (SIZE_T)PatchSize);
	bInstalled = true;

	if (OutTrampoline)
		*OutTrampoline = TrampolineMem;
	return true;
#else
	// Unsupported CPU family — hook is a no-op.
	(void)Target;
	(void)Detour;
	(void)OutTrampoline;
	return false;
#endif
}
bool FFunctionHook::Uninstall()
{
	if (!bInstalled) return false;

	{
		FScopedUnprotect Unprot(TargetAddr, (SIZE_T)PatchSize);
		FMemory::Memcpy(TargetAddr, OriginalBytes, PatchSize);
	}
	FlushIcache(TargetAddr, (SIZE_T)PatchSize);

	if (TrampolineMem)
	{
		const SIZE_T TrampSize = (SIZE_T)PatchSize + 16; // safe upper bound
		FreeExecutable(TrampolineMem, TrampSize);
		TrampolineMem = nullptr;
	}
	TargetAddr = nullptr;
	PatchSize = 0;
	bInstalled = false;
	return true;
}
