/*
 * alva_128_patches.cpp - RDR3 128-player patches (sites not covered by PR #3477).
 * Branch: ihatemylife-128 (local dev test rig).
 */

#include <StdInc.h>
#include <Hooking.h>
#include <Hooking.Stubs.h>

#include <jitasm.h>

// Site 1: sub_140A9F3B8 per-frame player-collect loop appends to a 32-slot buffer
// (struct+0x3100) with no bound check. Clamp the loop counter to 32.
static HookFunction hookFunction([]()
{
	// hook at `test ebx,ebx` (before the loop top) and clamp ebx to 32.
	auto location = hook::get_pattern("85 DB 0F 84 ? ? ? ? 44 8B F3");
	uintptr_t testInsn = reinterpret_cast<uintptr_t>(location);

	int32_t jzRel = *reinterpret_cast<int32_t*>(testInsn + 4);
	uintptr_t jzTarget = (testInsn + 8) + jzRel;

	static struct : jitasm::Frontend
	{
		uintptr_t retn;
		uintptr_t skipTarget;

		void Init(uintptr_t retnAddr, uintptr_t skip)
		{
			this->retn = retnAddr;
			this->skipTarget = skip;
		}

		virtual void InternalMain() override
		{
			cmp(ebx, 32);
			jbe("noClamp");
			mov(ebx, 32);
			L("noClamp");

			test(ebx, ebx);
			jz("skip");

			mov(rax, retn);
			jmp(rax);

			L("skip");
			mov(rax, skipTarget);
			jmp(rax);
		}
	} clampStub;

	const uintptr_t retnAddr = testInsn + 8; // mov r14d, ebx

	clampStub.Init(retnAddr, jzTarget);

	hook::nop(testInsn, 8);
	hook::jump(testInsn, clampStub.GetCode());
});

// Site 2: sub_1425A1E58 (sysTaskManager QueueWork) spin-hangs when its 2048-slot
// ring is full. Yield (SwitchToThread) on the full-queue retry path so workers drain.
extern "C" __declspec(dllimport) int __stdcall SwitchToThread(void);

static HookFunction taskQueueYieldHook([]()
{
	auto enq = reinterpret_cast<uintptr_t>(hook::get_pattern("44 8B 01 4C 8B C9 45 8B D0 41 81 E2 FF 07 00 00"));

	auto reload = enq + 0x30;             // mov r8d,[r9] ; jmp short loop_top
	const uintptr_t loopTop = enq + 0x06; // loop top (jmp target)

	// verify the reload block is `45 8B 01 EB` before patching.
	{
		const uint8_t* p = reinterpret_cast<const uint8_t*>(reload);
		if (!(p[0] == 0x45 && p[1] == 0x8B && p[2] == 0x01 && p[3] == 0xEB))
		{
			trace("[alva_128] task-queue yield: reload block mismatch (%02X %02X %02X %02X) - NOT patching\n",
				p[0], p[1], p[2], p[3]);
			return;
		}
	}

	static struct : jitasm::Frontend
	{
		uintptr_t loopTopAddr;

		void Init(uintptr_t lt) { this->loopTopAddr = lt; }

		virtual void InternalMain() override
		{
			push(rax); push(rcx); push(rdx); push(r8); push(r9); push(r10); push(r11);
			sub(rsp, 0x28);
			mov(rax, (uintptr_t)&SwitchToThread);
			call(rax);
			add(rsp, 0x28);
			pop(r11); pop(r10); pop(r9); pop(r8); pop(rdx); pop(rcx); pop(rax);

			mov(r8d, dword_ptr[r9]); // re-do the stolen reload

			mov(rax, loopTopAddr);
			jmp(rax);
		}
	} yieldStub;

	yieldStub.Init(loopTop);

	hook::nop(reload, 5); // steal `mov r8d,[r9]` (3) + `jmp short` (2)
	hook::jump(reload, yieldStub.GetCode());
});

// Site 3: the game-virtual arena is sized 692MB (0x2B400000) at startup and exhausts
// at ~128 players. Bump the hardcoded size immediate to 1GB.
static HookFunction heapSizeHook([]()
{
	auto movInsn = reinterpret_cast<uintptr_t>(
		hook::get_pattern("BA 00 00 40 2B 49 8B CE E8"));

	const uint8_t* p = reinterpret_cast<const uint8_t*>(movInsn);
	if (!(p[0] == 0xBA && p[1] == 0x00 && p[2] == 0x00 && p[3] == 0x40 && p[4] == 0x2B))
	{
		trace("[alva_128] heap-size: bytes mismatch at %p - NOT patching\n", (void*)movInsn);
		return;
	}

	hook::put<uint32_t>(movInsn + 1, 0x40000000); // 692MB -> 1GB

	trace("[alva_128] heap-size: game-virtual arena bumped 692MB -> 1GB\n");
});

// Site 4: sub_140A9F3B8 also appends 0x90-byte camera entries to a second 32-slot
// buffer (struct+0x3310, counter [+0x1200]) with no bound check; slot 32 overruns the
// adjacent job descriptor qword_1448E0190. Clamp the slot index to <= 31 at both
// append sites.
static HookFunction arrayBClampHook([]()
{
	auto matches = hook::pattern("48 8D 96 10 33 00 00 48 63 8A 00 12 00 00");

	const size_t count = matches.size();
	if (count == 0)
	{
		trace("[alva_128] arrayB-clamp: pattern not found - NOT patching\n");
		return;
	}

	for (size_t i = 0; i < count; i++)
	{
		uintptr_t leaInsn    = reinterpret_cast<uintptr_t>(matches.get(i).get<void>(0));
		uintptr_t movsxdInsn = leaInsn + 7; // movsxd rcx,[rdx+1200h]

		const uint8_t* p = reinterpret_cast<const uint8_t*>(movsxdInsn);
		if (!(p[0] == 0x48 && p[1] == 0x63 && p[2] == 0x8A &&
		      p[3] == 0x00 && p[4] == 0x12 && p[5] == 0x00 && p[6] == 0x00))
		{
			trace("[alva_128] arrayB-clamp: guard mismatch at %p - skipping\n", (void*)movsxdInsn);
			continue;
		}

		struct ClampStub : jitasm::Frontend
		{
			uintptr_t retn;
			void Init(uintptr_t r) { this->retn = r; }
			virtual void InternalMain() override
			{
				movsxd(rcx, dword_ptr[rdx + 0x1200]);
				cmp(ecx, 31);
				jle("keep");
				mov(ecx, 31);
				L("keep");
				mov(rax, retn);
				jmp(rax);
			}
		};

		ClampStub* stub = new ClampStub();
		stub->Init(movsxdInsn + 7);

		hook::nop(movsxdInsn, 7);
		hook::jump(movsxdInsn, stub->GetCode());

		trace("[alva_128] arrayB-clamp: patched append site at %p\n", (void*)movsxdInsn);
	}
});

// Sites 5/6: recursion-depth guard on the scope-tree walkers sub_142CFE1A8 (visitor)
// and sub_1425D7E8C (search). A cyclic tree recurses forever -> stack overflow. Cap
// the combined depth at 256 (well above any legitimate tree height) and bail safely.
static char (*g_origVisitor)(void* a1, void* a2);

static thread_local int g_treeWalkDepth = 0;
constexpr int kTreeWalkMaxDepth = 256;

// RAII so the counter always balances, even on exception unwind.
struct TreeDepthScope
{
	bool over;
	TreeDepthScope() { over = (g_treeWalkDepth >= kTreeWalkMaxDepth); if (!over) g_treeWalkDepth++; }
	~TreeDepthScope() { if (!over) g_treeWalkDepth--; }
};

static char VisitorGuarded(void* a1, void* a2)
{
	TreeDepthScope d;
	if (d.over)
	{
		static thread_local bool warned = false;
		if (!warned)
		{
			warned = true;
			trace("[alva_128] tree-guard: depth cap hit via visitor - bailing\n");
		}
		return 0;
	}

	return g_origVisitor(a1, a2);
}

static HookFunction visitorGuardHook([]()
{
	const char* sig = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B DA 48 8B F9 E8 ? ? ? ? 84 C0 74 13";

	if (hook::pattern(sig).size() == 0)
	{
		trace("[alva_128] visitor-guard: pattern not found - NOT guarding\n");
		return;
	}

	void* location = hook::get_pattern(sig);
	g_origVisitor = hook::trampoline(location, VisitorGuarded);
	trace("[alva_128] visitor-guard: installed at %p\n", location);
});

static void* (*g_origSearch)(void* node, void* a2, char a3);

static void* SearchGuarded(void* node, void* a2, char a3)
{
	TreeDepthScope d;
	if (d.over)
	{
		static thread_local bool warned = false;
		if (!warned)
		{
			warned = true;
			trace("[alva_128] tree-guard: depth cap hit via search - bailing\n");
		}
		return nullptr;
	}

	return g_origSearch(node, a2, a3);
}

static HookFunction searchGuardHook([]()
{
	const char* sig = "48 89 5C 24 10 57 48 83 EC 20 44 0F B7 59 68";

	if (hook::pattern(sig).size() == 0)
	{
		trace("[alva_128] search-guard: pattern not found - NOT guarding\n");
		return;
	}

	void* location = hook::get_pattern(sig);
	g_origSearch = hook::trampoline(location, SearchGuarded);
	trace("[alva_128] search-guard: installed at %p\n", location);
});

// Site 7: sub_142CFA1F4 dispatches `call [[r14]+0x58]` on a scene-tree node whose
// vtable ptr is garbage on a stale/recycled node at 40+ players -> wild jump. Validate
// the vtable ptr is in .rdata AND the call target [vt+0x58] is in .text; if not, skip
// the vcall (set al=1, jump to the skip landing) as the game does for [r14+8]==0.
static HookFunction vtableGuardHook([]()
{
	//   41 39 76 08   cmp  [r14+8], esi
	//   74 0D         jz   142CFA2BE
	//   49 8B 06      mov  rax, [r14]        <-- hook here (anchor+6)
	//   48 8D 55 D0   lea  rdx, [rbp-0x30]
	//   49 8B CE      mov  rcx, r14
	//   FF 50 58      call qword ptr [rax+58h]
	const char* sig = "41 39 76 08 74 0D 49 8B 06 48 8D 55 D0 49 8B CE FF 50 58";

	if (hook::pattern(sig).size() == 0)
	{
		trace("[alva_128] vtable-guard: pattern not found - NOT patching\n");
		return;
	}
	if (hook::pattern(sig).size() > 1)
	{
		trace("[alva_128] vtable-guard: pattern NOT UNIQUE (%zu) - NOT patching\n", hook::pattern(sig).size());
		return;
	}

	uintptr_t anchor   = reinterpret_cast<uintptr_t>(hook::get_pattern(sig));
	uintptr_t movRax   = anchor + 6;  // mov rax,[r14]
	uintptr_t callSite  = anchor + 16; // call [rax+0x58]
	uintptr_t skipTgt   = anchor + 19; // mov sil,al (skip landing)

	const uint8_t* p = reinterpret_cast<const uint8_t*>(movRax);
	if (!(p[0] == 0x49 && p[1] == 0x8B && p[2] == 0x06 &&
	      p[3] == 0x48 && p[4] == 0x8D && p[5] == 0x55 && p[6] == 0xD0 &&
	      p[7] == 0x49 && p[8] == 0x8B && p[9] == 0xCE))
	{
		trace("[alva_128] vtable-guard: steal-byte mismatch at %p - NOT patching\n", (void*)movRax);
		return;
	}

	// .rdata = valid vtable ptrs; .text = valid code (the call target).
	uintptr_t rdataLo = hook::get_adjusted(0x1432D8000ull);
	uintptr_t rdataHi = hook::get_adjusted(0x143917000ull);
	uintptr_t textLo  = hook::get_adjusted(0x140001000ull);
	uintptr_t textHi  = hook::get_adjusted(0x1432D8000ull);

	static struct : jitasm::Frontend
	{
		uintptr_t rlo, rhi, tlo, thi, callAddr, skipAddr;

		void Init(uintptr_t rl, uintptr_t rh, uintptr_t tl, uintptr_t th, uintptr_t c, uintptr_t s)
		{
			rlo = rl; rhi = rh; tlo = tl; thi = th; callAddr = c; skipAddr = s;
		}

		virtual void InternalMain() override
		{
			mov(rax, qword_ptr[r14]);

			// (1) vtable ptr in .rdata
			mov(r11, rlo);
			cmp(rax, r11);
			jb("bad");
			mov(r11, rhi);
			cmp(rax, r11);
			jae("bad");

			// (2) call target [vt+0x58] in .text
			mov(r11, qword_ptr[rax + 0x58]);
			mov(rdx, tlo);
			cmp(r11, rdx);
			jb("bad");
			mov(rdx, thi);
			cmp(r11, rdx);
			jae("bad");

			// good: re-do the stolen lea/mov and jump to the call.
			lea(rdx, qword_ptr[rbp - 0x30]);
			mov(rcx, r14);
			mov(r11, callAddr);
			jmp(r11);

			// bad: skip the vcall.
			L("bad");
			mov(al, 1);
			mov(r11, skipAddr);
			jmp(r11);
		}
	} guardStub;

	guardStub.Init(rdataLo, rdataHi, textLo, textHi, callSite, skipTgt);

	hook::nop(movRax, 10); // steal mov rax + lea rdx + mov rcx (3+4+3)
	hook::jump(movRax, guardStub.GetCode());

	trace("[alva_128] vtable-guard: installed at %p\n", (void*)movRax);
});

// Sites 7b-7e: same .rdata/.text vtable guard on the other indirect vcalls in the
// evaluator (sub_142CFA0D0 / sub_142CFA2E0). Per-site stubs because the object
// register, vtable slot, stolen bytes and skip landing differ. Each stub emits the
// call itself and jumps to the post-call landing (never back into the stolen bytes).
static HookFunction evaluatorVcallGuards([]()
{
	uintptr_t rdataLo = hook::get_adjusted(0x1432D8000ull);
	uintptr_t rdataHi = hook::get_adjusted(0x143917000ull);
	uintptr_t textLo  = hook::get_adjusted(0x140001000ull);
	uintptr_t textHi  = hook::get_adjusted(0x1432D8000ull);

	// 7b: mov rax,[rcx]; call [rax]  (obj=rcx, slot=0). skip -> post-call insn.
	{
		const char* sig = "89 5D 50 48 8B 01 FF 10";
		auto m = hook::pattern(sig);
		if (m.size() == 1)
		{
			uintptr_t anchor  = reinterpret_cast<uintptr_t>(m.get(0).get<void>(0));
			uintptr_t movVt   = anchor + 3;
			uintptr_t skipTgt = anchor + 8;

			static struct : jitasm::Frontend
			{
				uintptr_t rlo, rhi, tlo, thi, skipAddr;
				void Init(uintptr_t rl,uintptr_t rh,uintptr_t tl,uintptr_t th,uintptr_t s){rlo=rl;rhi=rh;tlo=tl;thi=th;skipAddr=s;}
				void InternalMain() override
				{
					// live at the call: rcx/rdx/r8/r9 - only clobber rax/r10/r11.
					mov(rax, qword_ptr[rcx]);
					mov(r11, rlo); cmp(rax, r11); jb("bad");
					mov(r11, rhi); cmp(rax, r11); jae("bad");
					mov(r11, qword_ptr[rax]);
					mov(r10, tlo); cmp(r11, r10); jb("bad");
					mov(r10, thi); cmp(r11, r10); jae("bad");
					call(r11);
					mov(r11, skipAddr); jmp(r11);
					L("bad");
					mov(al, 1); mov(r11, skipAddr); jmp(r11);
				}
			} s7b;
			s7b.Init(rdataLo, rdataHi, textLo, textHi, skipTgt);
			hook::nop(movVt, 5);
			hook::jump(movVt, s7b.GetCode());
			trace("[alva_128] vtable-guard 7b installed at %p\n", (void*)movVt);
		}
		else trace("[alva_128] vtable-guard 7b: sig matches=%zu - NOT patching\n", m.size());
	}

	// 7c: mov rdx,[rax]; mov rcx,rax; call [rdx+0x20]  (obj=rax, slot=0x20). skip -> fallthrough.
	{
		const char* sig = "48 8B 10 48 8B C8 FF 52 20";
		auto m = hook::pattern(sig);
		if (m.size() == 1)
		{
			uintptr_t anchor  = reinterpret_cast<uintptr_t>(m.get(0).get<void>(0));
			uintptr_t movVt   = anchor;
			uintptr_t skipTgt = anchor + 9;

			static struct : jitasm::Frontend
			{
				uintptr_t rlo, rhi, tlo, thi, skipAddr;
				void Init(uintptr_t rl,uintptr_t rh,uintptr_t tl,uintptr_t th,uintptr_t s){rlo=rl;rhi=rh;tlo=tl;thi=th;skipAddr=s;}
				void InternalMain() override
				{
					mov(rdx, qword_ptr[rax]);
					mov(r11, rlo); cmp(rdx, r11); jb("bad");
					mov(r11, rhi); cmp(rdx, r11); jae("bad");
					mov(r11, qword_ptr[rdx + 0x20]);
					mov(r10, tlo); cmp(r11, r10); jb("bad");
					mov(r10, thi); cmp(r11, r10); jae("bad");
					mov(rcx, rax);
					call(r11);
					mov(r11, skipAddr); jmp(r11);
					L("bad");
					mov(r11, skipAddr); jmp(r11);
				}
			} s7c;
			s7c.Init(rdataLo, rdataHi, textLo, textHi, skipTgt);
			hook::nop(movVt, 9);
			hook::jump(movVt, s7c.GetCode());
			trace("[alva_128] vtable-guard 7c installed at %p\n", (void*)movVt);
		}
		else trace("[alva_128] vtable-guard 7c: sig matches=%zu - NOT patching\n", m.size());
	}

	// 7d/7e: mov rax,[rbx]; lea rdx,[rbp-0x38]; mov rcx,rbx; call [rax+0x58]
	// (obj=rbx, slot=0x58). Two matches; patch both. skip -> post-call `mov dil,al`.
	{
		const char* sig = "48 8B 03 48 8D 55 C8 48 8B CB FF 50 58";
		auto m = hook::pattern(sig);
		size_t cnt = m.size();
		if (cnt >= 1)
		{
			for (size_t i = 0; i < cnt; i++)
			{
				uintptr_t anchor  = reinterpret_cast<uintptr_t>(m.get(i).get<void>(0));
				uintptr_t movVt   = anchor;
				uintptr_t skipTgt = anchor + 13;

				struct DEStub : jitasm::Frontend
				{
					uintptr_t rlo, rhi, tlo, thi, skipAddr;
					void Init(uintptr_t rl,uintptr_t rh,uintptr_t tl,uintptr_t th,uintptr_t s){rlo=rl;rhi=rh;tlo=tl;thi=th;skipAddr=s;}
					void InternalMain() override
					{
						mov(rax, qword_ptr[rbx]);
						mov(r11, rlo); cmp(rax, r11); jb("bad");
						mov(r11, rhi); cmp(rax, r11); jae("bad");
						mov(r11, qword_ptr[rax + 0x58]);
						mov(r10, tlo); cmp(r11, r10); jb("bad");
						mov(r10, thi); cmp(r11, r10); jae("bad");
						lea(rdx, qword_ptr[rbp - 0x38]);
						mov(rcx, rbx);
						call(r11);
						mov(r11, skipAddr); jmp(r11);
						L("bad");
						mov(al, 1); mov(r11, skipAddr); jmp(r11);
					}
				};
				DEStub* stub = new DEStub();
				stub->Init(rdataLo, rdataHi, textLo, textHi, skipTgt);
				hook::nop(movVt, 13);
				hook::jump(movVt, stub->GetCode());
				trace("[alva_128] vtable-guard 7d/e installed at %p\n", (void*)movVt);
			}
		}
		else trace("[alva_128] vtable-guard 7d/e: sig not found - NOT patching\n");
	}
});
