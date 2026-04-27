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
// Platform helpers: allocate RWX memory, change page permissions, flush icache
// ----------------------------------------------------------------------------

namespace FastBitCopyHookPrivate
{
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

	// -----------------------------------------------------------------------
	// x86_64 encoders
	// -----------------------------------------------------------------------
#if PLATFORM_CPU_X86_FAMILY
	// Write a 5-byte `E9 rel32` jmp at `At` targeting `To`.
	static void WriteJmpRel32(uint8* At, void* To)
	{
		int64 Rel = (int64)((intptr_t)To - (intptr_t)At - 5);
		At[0] = 0xE9;
		*(int32*)(At + 1) = (int32)Rel;
	}

	// Write a 14-byte absolute `FF 25 00 00 00 00 | imm64` jmp at `At` targeting `To`.
	static void WriteJmpAbs14(uint8* At, void* To)
	{
		At[0] = 0xFF; At[1] = 0x25;
		At[2] = 0x00; At[3] = 0x00; At[4] = 0x00; At[5] = 0x00;
		*(uint64*)(At + 6) = (uint64)(uintptr_t)To;
	}

	// Extremely small length-disassembler: just enough to figure out how many
	// bytes the first "at least N" instructions of a function occupy.
	// Handles the instruction families typically emitted as prologue:
	//   push/pop reg, mov reg,reg, sub/add reg,imm, lea, endbr64, nop,
	//   rex-prefix + above, 0F 1F .. (multi-byte NOPs), cld, etc.
	// If it sees something it doesn't understand it returns 0.
	static int InstrLen(const uint8* P)
	{
		int Len = 0;
		bool bHasOperandSize = false;
		bool bHasAddressSize = false;
		// prefixes
		while (true)
		{
			uint8 b = P[Len];
			if (b == 0x66) { bHasOperandSize = true; ++Len; continue; }
			if (b == 0x67) { bHasAddressSize = true; ++Len; continue; }
			if (b == 0xF0 || b == 0xF2 || b == 0xF3) { ++Len; continue; }
			if (b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65) { ++Len; continue; }
			break;
		}
		// REX
		bool bHasRex = false;
		if ((P[Len] & 0xF0) == 0x40) { bHasRex = true; ++Len; }

		uint8 Op = P[Len++];

		auto ModRMSize = [&](uint8 Mod, uint8 RM) -> int
		{
			int S = 1; // modrm itself
			if (Mod == 3) return S;
			bool bHasSIB = (RM == 4);
			if (bHasSIB) S += 1;
			if (Mod == 1) S += 1;
			else if (Mod == 2) S += 4;
			else if (Mod == 0 && RM == 5) S += 4; // rip-relative or disp32
			else if (Mod == 0 && bHasSIB)
			{
				uint8 SIB = P[Len];
				uint8 Base = SIB & 7;
				if (Base == 5) S += 4;
			}
			return S;
		};

		auto DecodeModRM = [&](int ImmSize)
		{
			uint8 MR = P[Len];
			uint8 Mod = (MR >> 6) & 3;
			uint8 RM  = MR & 7;
			Len += ModRMSize(Mod, RM);
			Len += ImmSize;
		};

		// Common opcodes
		switch (Op)
		{
		// 1-byte opcodes, no operand
		case 0x50: case 0x51: case 0x52: case 0x53:
		case 0x54: case 0x55: case 0x56: case 0x57: // push r64
		case 0x58: case 0x59: case 0x5A: case 0x5B:
		case 0x5C: case 0x5D: case 0x5E: case 0x5F: // pop r64
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
			default:
				return 0; // unsupported
			}
		}

		// MOV reg, imm32 (B8+rd)
		case 0xB8: case 0xB9: case 0xBA: case 0xBB:
		case 0xBC: case 0xBD: case 0xBE: case 0xBF:
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
		// 39 /r  cmp
		// 8A /r, 88 /r  mov byte
		case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8D:
		case 0x85: case 0x31: case 0x01: case 0x29: case 0x39:
		case 0x84: case 0x30: case 0x00: case 0x28: case 0x38:
			DecodeModRM(0);
			return Len;

		// FF /r  (push/call/jmp etc.) — we only accept push r/m64 (/6)
		case 0xFF:
		{
			uint8 MR = P[Len];
			uint8 Reg = (MR >> 3) & 7;
			if (Reg == 6) { DecodeModRM(0); return Len; } // push r/m
			return 0;
		}

		default:
			return 0; // unsupported — caller will bail out
		}
	}
#endif // PLATFORM_CPU_X86_FAMILY

	// -----------------------------------------------------------------------
	// aarch64 encoders
	// -----------------------------------------------------------------------
#if PLATFORM_CPU_ARM_FAMILY
	// Encode 16 bytes:
	//   LDR  X16, #8    ; 58000050
	//   BR   X16        ; D61F0200
	//   <imm64>
	static void WriteJmpAbs16(uint8* At, void* To)
	{
		*(uint32*)(At + 0) = 0x58000050u;
		*(uint32*)(At + 4) = 0xD61F0200u;
		*(uint64*)(At + 8) = (uint64)(uintptr_t)To;
	}

	// Reject instructions whose encoding is PC-relative and therefore would
	// break when relocated: B, BL, B.cond, CBZ/CBNZ, TBZ/TBNZ, ADR, ADRP,
	// and LDR (literal). We only need to validate the first 4 instructions.
	static bool IsRelocatableInsn(uint32 Insn)
	{
		// B       000101 imm26            -> top 6 bits 000101 (0x14000000..)
		// BL      100101 imm26            -> top 6 bits 100101 (0x94000000..)
		if ((Insn & 0x7C000000u) == 0x14000000u) return false; // B / BL
		// B.cond  01010100 .............. -> 0x54xxxxxx
		if ((Insn & 0xFF000010u) == 0x54000000u) return false;
		// CBZ/CBNZ  x0110100 / x0110101
		if ((Insn & 0x7E000000u) == 0x34000000u) return false;
		// TBZ/TBNZ  x0110110 / x0110111
		if ((Insn & 0x7E000000u) == 0x36000000u) return false;
		// ADR / ADRP  0xx10000 ...
		if ((Insn & 0x1F000000u) == 0x10000000u) return false;
		// LDR (literal)  0x011000 ...  (01011000 / 00011000 / 10011000 / ...)
		if ((Insn & 0x3B000000u) == 0x18000000u) return false;
		return true;
	}
#endif
} // namespace FastBitCopyHookPrivate

using namespace FastBitCopyHookPrivate;

FFunctionHook::FFunctionHook()  = default;
FFunctionHook::~FFunctionHook() { if (bInstalled) Uninstall(); }

bool FFunctionHook::Install(void* Target, void* Detour, void** OutTrampoline)
{
	check(Target);
	check(Detour);
	if (bInstalled) return false;

#if PLATFORM_CPU_X86_FAMILY
	// Step 1: figure out how many bytes of prologue we need to relocate.
	uint8* T = (uint8*)Target;
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
			return false;
		}
		Copied += N;
	}

	PatchSize = Copied;
	check(PatchSize <= (int32)sizeof(OriginalBytes));
	FMemory::Memcpy(OriginalBytes, T, PatchSize);

	// Step 2: allocate an executable trampoline (PatchSize + 14 bytes for abs jump back)
	const SIZE_T TrampSize = (SIZE_T)PatchSize + 14;
	void* Tramp = AllocExecutable(Target, TrampSize);
	if (!Tramp) return false;
	FMemory::Memcpy(Tramp, OriginalBytes, PatchSize);
	WriteJmpAbs14((uint8*)Tramp + PatchSize, (uint8*)Target + PatchSize);

	// Step 3: patch the prologue.
	{
		FScopedUnprotect Unprot(Target, (SIZE_T)PatchSize);
		int64 Rel = (int64)((intptr_t)Detour - (intptr_t)Target - 5);
		if (Needed == 5 && Rel >= INT32_MIN && Rel <= INT32_MAX)
		{
			WriteJmpRel32(T, Detour);
			// Pad the rest with NOPs so a debugger / profiler sees clean insns.
			for (int32 i = 5; i < PatchSize; ++i) T[i] = 0x90;
		}
		else
		{
			// Absolute 14-byte jump.
			if (PatchSize < 14)
			{
				// We need 14 bytes but only decoded fewer; extend by decoding more
				while (Copied < 14)
				{
					int N = InstrLen(T + Copied);
					if (N <= 0) { FreeExecutable(Tramp, TrampSize); return false; }
					Copied += N;
				}
				// Reallocate trampoline with new PatchSize
				FreeExecutable(Tramp, TrampSize);
				PatchSize = Copied;
				FMemory::Memcpy(OriginalBytes, T, PatchSize);
				const SIZE_T TrampSize2 = (SIZE_T)PatchSize + 14;
				Tramp = AllocExecutable(Target, TrampSize2);
				if (!Tramp) return false;
				FMemory::Memcpy(Tramp, OriginalBytes, PatchSize);
				WriteJmpAbs14((uint8*)Tramp + PatchSize, (uint8*)Target + PatchSize);
			}
			WriteJmpAbs14(T, Detour);
			for (int32 i = 14; i < PatchSize; ++i) T[i] = 0x90;
		}
	}
	FlushIcache(Target, (SIZE_T)PatchSize);
	FlushIcache(Tramp, TrampSize);

	TargetAddr    = Target;
	TrampolineMem = Tramp;
	if (OutTrampoline) *OutTrampoline = Tramp;
	bInstalled = true;
	return true;

#elif PLATFORM_CPU_ARM_FAMILY
	// Validate and copy the first 16 bytes (4 instructions).
	const int32 kPatch = 16;
	uint32* T32 = (uint32*)Target;
	for (int32 i = 0; i < 4; ++i)
	{
		if (!IsRelocatableInsn(T32[i]))
		{
			return false;
		}
	}
	PatchSize = kPatch;
	FMemory::Memcpy(OriginalBytes, Target, kPatch);

	const SIZE_T TrampSize = (SIZE_T)kPatch + 16; // prologue + abs-16 jump back
	void* Tramp = AllocExecutable(Target, TrampSize);
	if (!Tramp) return false;

	{
		// Trampoline is already RWX (or we need to toggle on Apple Silicon via FScopedUnprotect
		// logic — AllocExecutable requested RX on mac-arm64). So unprotect to write.
		FScopedUnprotect Unprot(Tramp, TrampSize, /*bIsJitPage=*/true);
		FMemory::Memcpy(Tramp, OriginalBytes, kPatch);
		WriteJmpAbs16((uint8*)Tramp + kPatch, (uint8*)Target + kPatch);
	}
	FlushIcache(Tramp, TrampSize);

	{
		FScopedUnprotect Unprot(Target, (SIZE_T)kPatch);
		WriteJmpAbs16((uint8*)Target, Detour);
	}
	FlushIcache(Target, (SIZE_T)kPatch);

	TargetAddr    = Target;
	TrampolineMem = Tramp;
	if (OutTrampoline) *OutTrampoline = Tramp;
	bInstalled = true;
	return true;

#else
	(void)Target; (void)Detour; (void)OutTrampoline;
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
