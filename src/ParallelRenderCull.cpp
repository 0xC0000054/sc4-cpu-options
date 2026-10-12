/*
 *  This file is part of sc4-cpu-options. It comes from SCD3D11, a free Direct3D 11
 *  driver for SimCity 4's SimGL interface (https://github.com/UlisseWolf/scd3d11).
 *  Copyright (C) 2026 Casper Van Gheluwe
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// cSC43DRender::DrawStaticView / DrawDynamicView run in three phases: gather
// (walk the quad grid, frustum-test elements, build a sort-key array), sort,
// draw. The gather is pure per-element math plus one const vtable getter and is
// parallel across quad columns. This detours the two functions, runs the gather
// (serial or pooled) into a pre-sort array identical to the stock one, writes it
// into the render's own vectors, then jumps into the untouched original tail
// (a naked thunk rebuilds the frame) for sort + draw.
//
// Any surprise (wrong build, null grid, vector too small) falls back to the
// original. Capacity misses self-heal as the fallback grows the vector.
//
// Target: SimCity 4 Deluxe 1.1.641 (Windows), image base 0x00400000.

#include "ParallelRenderCull.h"

#include "GameVersion.h"
#include "Logger.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <Windows.h>

namespace ParallelRenderCull
{
	namespace
	{

		constexpr uintptr_t kImageBase = 0x00400000;

		constexpr uintptr_t kDrawStaticViewVA = 0x007C7370;
		constexpr uintptr_t kDrawStaticViewTailVA = 0x007C7657;
		constexpr uintptr_t kDrawDynamicViewVA = 0x007C7830;
		constexpr uintptr_t kDrawDynamicViewTailVA = 0x007C7BF8;

		constexpr uintptr_t kGetVisibleContainerRectVA = 0x007C1FD0;
		constexpr uintptr_t kComputeViewportZProjectionVA = 0x007C2F80;
		constexpr uintptr_t kGetQuadrantsOfScreenRectVA = 0x005F29C0;
		constexpr uintptr_t kSetForDepthSortVA = 0x007C2EB0;

		// push ebp / mov ebp,esp / and esp,-8 / sub esp,<70|78>
		constexpr size_t kRelocatedBytes = 6;
		constexpr size_t kJumpSize = 5;
		const uint8_t kPrologue[8] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC };
		const uint8_t kStaticFrameByte = 0x70;
		const uint8_t kDynamicFrameByte = 0x78;

		// cSC43DRender offsets
		constexpr size_t kOff_ViewUtils = 0x114;
		constexpr size_t kOff_DrawCtx = 0x11C;
		constexpr size_t kOff_QuadGrid = 0x120;
		constexpr size_t kOff_Billboards = 0x128; // vector<SC4GridElement*>
		constexpr size_t kOff_Buf0 = 0x134;       // vector<cSC4ModelSortElement>
		constexpr size_t kOff_Buf1 = 0x140;
		constexpr size_t kOff_Buf2 = 0x14C;

		// SC4GridElement offsets
		constexpr size_t kElem_Flags = 0x06;   // uint16
		constexpr size_t kElem_HomeCol = 0x08; // int32
		constexpr size_t kElem_HomeRow = 0x0C; // int32
		constexpr size_t kElem_ViewObj = 0x18; // cS3DModelInstance*

		// SC4QuadGrid: [0x14] -> outer[] (stride 0xC, [0] -> row[]); row stride 0x10,
		// [0]/[4] = elem** begin/end, [0xC] = uint16 prune flags.
		constexpr size_t kGrid_OuterArray = 0x14;
		constexpr size_t kGridOuterStride = 0x0C;
		constexpr size_t kGridQuadStride = 0x10;

		constexpr size_t kViewObjVtGetSortValue = 0x44; // cS3DModelInstance::GetSortValue

		struct SortEl
		{
			void* model;
			int32_t key;
		};
		static_assert(sizeof(SortEl) == 8, "cSC4ModelSortElement is 8 bytes");

		struct RawVec
		{
			uint8_t* begin;
			uint8_t* end;
			uint8_t* cap;
		};

		using GetVisibleContainerRectFn = void(__thiscall*)(void*, float*, int);
		using ComputeViewportZProjectionFn = void(__cdecl*)(float*, void*);
		using GetQuadrantsOfScreenRectFn = void(__thiscall*)(void*, float*, int*, int*, int*, int*);
		using SetForDepthSortFn = void(__thiscall*)(void*, void*, float*);
		using GetSortValueFn = void(__thiscall*)(void*, uint32_t*);
		using DrawViewFn = void(__fastcall*)(void*);

		GetVisibleContainerRectFn GetVisibleContainerRect = nullptr;
		ComputeViewportZProjectionFn ComputeViewportZProjection = nullptr;
		GetQuadrantsOfScreenRectFn GetQuadrantsOfScreenRect = nullptr;
		SetForDepthSortFn SetForDepthSort = nullptr;

		uintptr_t gStaticTailTarget = 0;  // read by StaticTailThunk
		uintptr_t gDynamicTailTarget = 0; // read by DynamicTailThunk

		Mode gMode = Mode::Parallel;
		std::atomic<uint32_t> gStaticCalls{ 0 };
		std::atomic<uint32_t> gDynamicCalls{ 0 };

		// Set if a worker range faults; RunDetour then falls back to the stock path.
		std::atomic<uint32_t> gWorkerFaultCode{ 0 };
		std::atomic<void*> gWorkerFaultAddr{ nullptr };

		int WorkerFilter(uint32_t code, _EXCEPTION_POINTERS* ep)
		{
			gWorkerFaultCode.store(code);
			gWorkerFaultAddr.store(ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr);
			return EXCEPTION_EXECUTE_HANDLER;
		}

		void SafeInvoke(const std::function<void(unsigned)>& body, unsigned i)
		{
			__try
			{
				body(i);
			}
			__except (WorkerFilter(GetExceptionCode(), GetExceptionInformation()))
			{
			}
		}

		// ---- worker pool --------------------------------------------------

		class WorkerPool
		{
		public:
			// affinity is the processors the workers may run on, 0 for anywhere.
			void Start(unsigned workers, uintptr_t affinity)
			{
				stop_ = false;
				generation_ = 0;
				threads_.reserve(workers);
				for (unsigned i = 0; i < workers; ++i)
					threads_.emplace_back([this, affinity]
						{
							if (affinity != 0) SetThreadAffinityMask(GetCurrentThread(), affinity);
							Loop();
						});
			}

			void Stop()
			{
				{
					std::lock_guard<std::mutex> lock(mutex_);
					stop_ = true;
					++generation_;
				}
				wake_.notify_all();
				for (auto& t : threads_)
					if (t.joinable()) t.join();
				threads_.clear();
			}

			unsigned Size() const { return static_cast<unsigned>(threads_.size()); }

			// body(i) for i in [0,ranges); caller participates; blocks until done.
			void Run(unsigned ranges, const std::function<void(unsigned)>& body)
			{
				if (ranges == 0) return;
				if (threads_.empty() || ranges == 1)
				{
					for (unsigned i = 0; i < ranges; ++i) body(i);
					return;
				}
				{
					std::lock_guard<std::mutex> lock(mutex_);
					job_ = &body;
					jobRanges_ = ranges;
					nextRange_.store(0, std::memory_order_relaxed);
					pending_ = static_cast<int>(threads_.size());
					++generation_;
				}
				wake_.notify_all();
				Drain();
				std::unique_lock<std::mutex> lock(mutex_);
				done_.wait(lock, [this] { return pending_ == 0; });
				job_ = nullptr;
			}

		private:
			void Loop()
			{
				uint64_t seen = 0;
				for (;;)
				{
					std::unique_lock<std::mutex> lock(mutex_);
					wake_.wait(lock, [this, &seen] { return stop_ || generation_ != seen; });
					seen = generation_;
					if (stop_) return;
					lock.unlock();
					Drain();
					lock.lock();
					if (--pending_ == 0)
					{
						lock.unlock();
						done_.notify_one();
					}
				}
			}

			void Drain()
			{
				const auto& body = *job_;
				const unsigned total = jobRanges_;
				for (;;)
				{
					unsigned i = nextRange_.fetch_add(1, std::memory_order_relaxed);
					if (i >= total) return;
					SafeInvoke(body, i);
				}
			}

			std::vector<std::thread> threads_;
			std::mutex mutex_;
			std::condition_variable wake_;
			std::condition_variable done_;
			bool stop_ = true;
			uint64_t generation_ = 0;
			const std::function<void(unsigned)>* job_ = nullptr;
			unsigned jobRanges_ = 0;
			std::atomic<unsigned> nextRange_{ 0 };
			int pending_ = 0;
		};

		WorkerPool gPool;

		// ---- inline hook plumbing ---------------------------------------

		struct Detour
		{
			uint8_t* target = nullptr;
			uint8_t* trampoline = nullptr;
			uint8_t saved[kRelocatedBytes]{};
			bool installed = false;
		};

		Detour gStaticDetour;
		Detour gDynamicDetour;

		void WriteRelJump(uint8_t* at, const void* dest)
		{
			at[0] = 0xE9;
			const int32_t disp = static_cast<int32_t>(reinterpret_cast<uintptr_t>(dest) -
				(reinterpret_cast<uintptr_t>(at) + kJumpSize));
			std::memcpy(at + 1, &disp, sizeof(disp));
		}

		bool InstallDetour(Detour& d, uintptr_t targetVA, uint8_t frameByte, void* replacement)
		{
			auto module = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
			if (module == nullptr) return false;
			d.target = module + (targetVA - kImageBase);

			if (std::memcmp(d.target, kPrologue, sizeof(kPrologue)) != 0 || d.target[8] != frameByte)
			{
				Logger::GetInstance().WriteLineFormatted(LogLevel::Info, "parallel cull: %08X prologue mismatch, skipping",
					static_cast<unsigned>(targetVA));
				d.target = nullptr;
				return false;
			}

			d.trampoline = static_cast<uint8_t*>(VirtualAlloc(
				nullptr, kRelocatedBytes + kJumpSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
			if (d.trampoline == nullptr)
			{
				d.target = nullptr;
				return false;
			}
			std::memcpy(d.trampoline, d.target, kRelocatedBytes);
			WriteRelJump(d.trampoline + kRelocatedBytes, d.target + kRelocatedBytes);
			FlushInstructionCache(GetCurrentProcess(), d.trampoline, kRelocatedBytes + kJumpSize);
			std::memcpy(d.saved, d.target, kRelocatedBytes);

			DWORD prot = 0;
			if (!VirtualProtect(d.target, kRelocatedBytes, PAGE_EXECUTE_READWRITE, &prot))
			{
				VirtualFree(d.trampoline, 0, MEM_RELEASE);
				d.trampoline = nullptr;
				d.target = nullptr;
				return false;
			}
			WriteRelJump(d.target, replacement);
			d.target[kJumpSize] = 0x90;
			DWORD ignored = 0;
			VirtualProtect(d.target, kRelocatedBytes, prot, &ignored);
			FlushInstructionCache(GetCurrentProcess(), d.target, kRelocatedBytes);
			d.installed = true;
			return true;
		}

		void RemoveDetour(Detour& d)
		{
			if (!d.installed) return;
			DWORD prot = 0;
			if (VirtualProtect(d.target, kRelocatedBytes, PAGE_EXECUTE_READWRITE, &prot))
			{
				std::memcpy(d.target, d.saved, kRelocatedBytes);
				DWORD ignored = 0;
				VirtualProtect(d.target, kRelocatedBytes, prot, &ignored);
				FlushInstructionCache(GetCurrentProcess(), d.target, kRelocatedBytes);
			}
			if (d.trampoline) VirtualFree(d.trampoline, 0, MEM_RELEASE);
			d.trampoline = nullptr;
			d.installed = false;
		}

		// ---- tail thunks ----------------------------------------------
		// Rebuild the exact frame the stock function holds at the sort, then jump
		// there. Frame: push ebp / mov ebp,esp / and esp,-8 / sub esp,<f> /
		// push ebx,ebp,esi,edi, plus the one slot the re-queue path reloads this
		// from ([esp+0x1C] static, [esp+0x14] dynamic); static tail also wants
		// esi = this+0x134.

		__declspec(naked) void __fastcall StaticTailThunk(void* /*ecx=this*/)
		{
			__asm {
				push ebp
				mov  ebp, esp
				and esp, -8
				sub  esp, 0x70
				push ebx
				push ebp
				push esi
				push edi
				mov  edi, ecx
				mov  dword ptr[esp + 0x1C], ecx
				lea  esi, [edi + 0x134]
				mov  eax, gStaticTailTarget
				jmp  eax
			}
		}

		__declspec(naked) void __fastcall DynamicTailThunk(void* /*ecx=this*/)
		{
			__asm {
				push ebp
				mov  ebp, esp
				and esp, -8
				sub  esp, 0x78
				push ebx
				push ebp
				push esi
				push edi
				mov  edi, ecx
				mov  dword ptr[esp + 0x14], ecx
				mov  eax, gDynamicTailTarget
				jmp  eax
			}
		}

		// ---- gather ---------------------------------------------------

		inline void ClearVec(RawVec* v) { v->end = v->begin; }
		inline size_t CapElems(const RawVec* v, size_t sz)
		{
			return static_cast<size_t>(v->cap - v->begin) / sz;
		}

		inline int32_t FoldSortValue(void* viewObj, int32_t depthKey)
		{
			uint32_t sv = 0;
			auto vt = *reinterpret_cast<void***>(viewObj);
			reinterpret_cast<GetSortValueFn>(vt[kViewObjVtGetSortValue / sizeof(void*)])(viewObj, &sv);
			return static_cast<int32_t>(static_cast<uint32_t>(depthKey) + sv * 0x10000u + (sv & 0xFFFF0000u));
		}

		inline uint8_t* RowArray(void* grid, int col)
		{
			auto outer = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(grid) + kGrid_OuterArray);
			if (outer == nullptr) return nullptr;
			return *reinterpret_cast<uint8_t**>(outer + static_cast<size_t>(col) * kGridOuterStride);
		}

		void GatherStaticColumn(void* grid, float* zproj, int col, int c0, int r0, int r1,
			std::vector<SortEl>& out)
		{
			uint8_t* rows = RowArray(grid, col);
			if (rows == nullptr) return;
			const int32_t colAcc = col - c0 - 1;
			for (int row = r0; row <= r1; ++row)
			{
				auto quad = rows + static_cast<size_t>(row) * kGridQuadStride;
				auto e = *reinterpret_cast<void***>(quad);
				auto eEnd = *reinterpret_cast<void***>(quad + 4);
				const int32_t rowAcc = row - r0 - 1;
				for (; e != eEnd; ++e)
				{
					auto elem = reinterpret_cast<uint8_t*>(*e);
					if ((*reinterpret_cast<uint16_t*>(elem + kElem_Flags) & 8) == 0) continue;
					const int32_t hr = *reinterpret_cast<int32_t*>(elem + kElem_HomeRow);
					const int32_t hc = *reinterpret_cast<int32_t*>(elem + kElem_HomeCol);
					const uint32_t x = (static_cast<uint32_t>(row - hr) - 1u) | static_cast<uint32_t>(rowAcc);
					const uint32_t y = (static_cast<uint32_t>(col - hc) - 1u) | static_cast<uint32_t>(colAcc);
					if (static_cast<int32_t>(x & y) >= 0) continue;
					SortEl s;
					SetForDepthSort(&s, elem, zproj);
					s.key = FoldSortValue(*reinterpret_cast<void**>(elem + kElem_ViewObj), s.key);
					out.push_back(s);
				}
			}
		}

		void GatherDynamicColumn(void* grid, float* zproj, int col, int c0, int r0, int r1,
			std::vector<SortEl>& b0, std::vector<SortEl>& b1,
			std::vector<SortEl>& b2, std::vector<void*>& bb)
		{
			uint8_t* rows = RowArray(grid, col);
			if (rows == nullptr) return;
			const int32_t colAcc = col - c0 - 1;
			for (int row = r0; row <= r1; ++row)
			{
				auto quad = rows + static_cast<size_t>(row) * kGridQuadStride;
				auto e = *reinterpret_cast<void***>(quad);
				auto eEnd = *reinterpret_cast<void***>(quad + 4);
				if (e == eEnd) continue;
				auto quadFlags = reinterpret_cast<uint16_t*>(quad + 0x0C);
				if ((*quadFlags & 1) == 0) continue;

				uint16_t newFlags = static_cast<uint16_t>(*quadFlags & 0xFFFE);
				const int32_t rowAcc = row - r0 - 1;
				for (; e != eEnd; ++e)
				{
					auto elem = reinterpret_cast<uint8_t*>(*e);
					const uint16_t ef = *reinterpret_cast<uint16_t*>(elem + kElem_Flags);
					const bool selfLit = static_cast<int8_t>(ef & 0xFF) < 0; // 0x80
					if ((ef & 8) != 0 && !selfLit) continue;

					const int32_t hr = *reinterpret_cast<int32_t*>(elem + kElem_HomeRow);
					const int32_t hc = *reinterpret_cast<int32_t*>(elem + kElem_HomeCol);
					const uint32_t x = (static_cast<uint32_t>(row - hr) - 1u) | static_cast<uint32_t>(rowAcc);
					const uint32_t y = (static_cast<uint32_t>(col - hc) - 1u) | static_cast<uint32_t>(colAcc);
					if (static_cast<int32_t>(x & y) < 0)
					{
						if ((ef & 8) == 0)
						{
							SortEl s;
							SetForDepthSort(&s, elem, zproj);
							if (ef & 1)
							{
								b2.push_back(s);
							}
							else if (ef & 0x100)
							{
								b1.push_back(s);
							}
							else
							{
								s.key = FoldSortValue(*reinterpret_cast<void**>(elem + kElem_ViewObj), s.key);
								b0.push_back(s);
							}
						}
						if (selfLit) bb.push_back(elem);
					}
					newFlags |= 1;
				}
				*quadFlags = newFlags;
			}
		}

		struct Span
		{
			int lo, hi;
		};
		Span SpanFor(unsigned idx, unsigned ranges, int c0, int c1)
		{
			const int total = c1 - c0 + 1;
			const int base = total / static_cast<int>(ranges);
			const int extra = total % static_cast<int>(ranges);
			const int i = static_cast<int>(idx);
			const int lo = c0 + i * base + (i < extra ? i : extra);
			const int count = base + (i < extra ? 1 : 0);
			return { lo, lo + count - 1 };
		}

		// ---- detours ------------------------------------------------

		// Below this many gathered elements the worker-wake overhead isn't worth
		// it; gather on the render thread instead. Seeded from the previous frame.
		constexpr size_t kMinParallelWork = 600;

		struct ViewState
		{
			std::vector<std::vector<SortEl>> b0, b1, b2;
			std::vector<std::vector<void*>> bb;
			size_t lastTotal = 0;
			std::atomic<uint32_t> fast{ 0 }, fallback{ 0 }, faults{ 0 };

			void Ensure(unsigned ranges)
			{
				auto e = [ranges](auto& v)
					{
						if (v.size() < ranges) v.resize(ranges);
						for (auto& p : v) p.clear();
					};
				e(b0);
				e(b1);
				e(b2);
				e(bb);
			}
		};
		ViewState gStatic, gDynamic;

		template <bool Dynamic>
		void RunDetour(void* self)
		{
			Detour& d = Dynamic ? gDynamicDetour : gStaticDetour;
			auto original = reinterpret_cast<DrawViewFn>(d.trampoline);
			if (gMode == Mode::Off)
			{
				original(self);
				return;
			}

			ViewState& vs = Dynamic ? gDynamic : gStatic;
			uint32_t call = (Dynamic ? gDynamicCalls : gStaticCalls).fetch_add(1);
			if (call == 0)
				Logger::GetInstance().WriteLineFormatted(LogLevel::Debug, "parallel cull: first %s detour", Dynamic ? "dynamic" : "static");

			if (gMode == Mode::Passthru)
			{
				original(self);
				return;
			}

			auto base = reinterpret_cast<uint8_t*>(self);
			void* drawCtx = *reinterpret_cast<void**>(base + kOff_DrawCtx);
			void* viewUtils = *reinterpret_cast<void**>(base + kOff_ViewUtils);
			void* grid = *reinterpret_cast<void**>(base + kOff_QuadGrid);

			*reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(drawCtx) + 0x50) = Dynamic ? 4 : 1;

			auto* buf0 = reinterpret_cast<RawVec*>(base + kOff_Buf0);
			auto* buf1 = reinterpret_cast<RawVec*>(base + kOff_Buf1);
			auto* buf2 = reinterpret_cast<RawVec*>(base + kOff_Buf2);
			auto* bbv = reinterpret_cast<RawVec*>(base + kOff_Billboards);

			ClearVec(buf0);
			ClearVec(buf1);
			if (Dynamic)
			{
				ClearVec(buf2);
				ClearVec(bbv);
			}

			float rect[4];
			GetVisibleContainerRect(self, rect, Dynamic ? 2 : 1);
			float zproj[4];
			ComputeViewportZProjection(zproj, viewUtils);

			if (grid == nullptr)
			{
				original(self);
				return;
			}
			if (gMode == Mode::TailOnly)
			{
				(Dynamic ? DynamicTailThunk : StaticTailThunk)(self);
				return;
			}

			int c0 = 0, r0 = 0, c1 = 0, r1 = 0;
			GetQuadrantsOfScreenRect(grid, rect, &c0, &r0, &c1, &r1);
			if (c0 > c1 || r0 > r1)
			{
				(Dynamic ? DynamicTailThunk : StaticTailThunk)(self);
				return;
			}

			const unsigned columns = static_cast<unsigned>(c1 - c0 + 1);
			unsigned ranges = 1;
			if (gMode == Mode::Parallel && gPool.Size() > 0 && columns >= 2 &&
				vs.lastTotal >= kMinParallelWork)
			{
				ranges = gPool.Size() + 1;
				if (ranges > columns) ranges = columns;
			}

			gWorkerFaultCode.store(0);
			vs.Ensure(ranges);

			gPool.Run(ranges, [&](unsigned i)
				{
					Span s = SpanFor(i, ranges, c0, c1);
					for (int col = s.lo; col <= s.hi; ++col)
					{
						if (Dynamic)
							GatherDynamicColumn(grid, zproj, col, c0, r0, r1, vs.b0[i], vs.b1[i], vs.b2[i], vs.bb[i]);
						else
							GatherStaticColumn(grid, zproj, col, c0, r0, r1, vs.b0[i]);
					}
				});

			size_t n0 = 0, n1 = 0, n2 = 0, nbb = 0;
			for (unsigned i = 0; i < ranges; ++i)
			{
				n0 += vs.b0[i].size();
				if (Dynamic)
				{
					n1 += vs.b1[i].size();
					n2 += vs.b2[i].size();
					nbb += vs.bb[i].size();
				}
			}
			vs.lastTotal = n0 + n1 + n2;

			bool overflow = n0 > CapElems(buf0, sizeof(SortEl));
			if (Dynamic)
				overflow = overflow || n1 > CapElems(buf1, sizeof(SortEl)) ||
				n2 > CapElems(buf2, sizeof(SortEl)) || nbb > CapElems(bbv, sizeof(void*));

			if (uint32_t fc = gWorkerFaultCode.load())
			{
				if (vs.faults.fetch_add(1) < 4)
					Logger::GetInstance().WriteLineFormatted(LogLevel::Error, "parallel cull: %s worker fault 0x%08X at %p, falling back",
						Dynamic ? "dynamic" : "static", fc, gWorkerFaultAddr.load());
				original(self);
				return;
			}
			if (overflow)
			{
				// Vector lacked spare capacity; the stock path grows it and the
				// next redraw fits.
				vs.fallback.fetch_add(1);
				original(self);
				return;
			}

			uint32_t f = vs.fast.fetch_add(1) + 1;
			if ((f & (f - 1)) == 0)
				Logger::GetInstance().WriteLineFormatted(LogLevel::Debug,
					"parallel cull: %s fast #%u ranges=%u total=%zu fallbacks=%u",
					Dynamic ? "dynamic" : "static", f, ranges, n0 + n1 + n2, vs.fallback.load());

			auto concat = [&](RawVec* v, std::vector<std::vector<SortEl>>& parts)
				{
					auto* p = reinterpret_cast<SortEl*>(v->begin);
					for (unsigned i = 0; i < ranges; ++i)
						if (!parts[i].empty())
						{
							std::memcpy(p, parts[i].data(), parts[i].size() * sizeof(SortEl));
							p += parts[i].size();
						}
					v->end = reinterpret_cast<uint8_t*>(p);
				};
			concat(buf0, vs.b0);
			if (Dynamic)
			{
				concat(buf1, vs.b1);
				concat(buf2, vs.b2);
				auto* p = bbv->begin;
				for (unsigned i = 0; i < ranges; ++i)
					if (!vs.bb[i].empty())
					{
						std::memcpy(p, vs.bb[i].data(), vs.bb[i].size() * sizeof(void*));
						p += vs.bb[i].size() * sizeof(void*);
					}
				bbv->end = p;
			}

			(Dynamic ? DynamicTailThunk : StaticTailThunk)(self);
		}

		void __fastcall DetourStaticView(void* self, void* /*edx*/) { RunDetour<false>(self); }
		void __fastcall DetourDynamicView(void* self, void* /*edx*/) { RunDetour<true>(self); }

		// ---- install ----------------------------------------------


		bool gInstalled = false;

	} // namespace

	Mode ParseMode(const char* v)
	{
		if (_stricmp(v, "0") == 0 || _stricmp(v, "off") == 0 || _stricmp(v, "false") == 0) return Mode::Off;
		if (_stricmp(v, "passthru") == 0) return Mode::Passthru;
		if (_stricmp(v, "tailonly") == 0) return Mode::TailOnly;
		if (_stricmp(v, "serial") == 0) return Mode::Serial;
		return Mode::Parallel;
	}

	bool Install(unsigned workers, uintptr_t workerAffinity, Mode requested)
	{
		if (gInstalled) return true;

		if (requested == Mode::Off)
		{
			Logger::GetInstance().WriteLine(LogLevel::Info, "parallel cull: turned off");
			return false;
		}
		if (workers > kMaxWorkers) workers = kMaxWorkers;
		// With fewer than two workers to share the gather, the detour costs about what it saves.
		if (requested == Mode::Parallel && workers < 2)
		{
			Logger::GetInstance().WriteLineFormatted(LogLevel::Info,
				"parallel cull: only %u logical processor(s) free for workers, skipping", workers);
			return false;
		}
		if (!GameVersion::IsSupportedForPatches())
		{
			Logger::GetInstance().WriteLineFormatted(LogLevel::Info, "parallel cull: requires SimCity 4 1.1.641, skipping");
			return false;
		}

		auto module = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
		if (module == nullptr) return false;
		auto rebase = [&](uintptr_t va) { return reinterpret_cast<void*>(module + (va - kImageBase)); };

		GetVisibleContainerRect = reinterpret_cast<GetVisibleContainerRectFn>(rebase(kGetVisibleContainerRectVA));
		ComputeViewportZProjection = reinterpret_cast<ComputeViewportZProjectionFn>(rebase(kComputeViewportZProjectionVA));
		GetQuadrantsOfScreenRect = reinterpret_cast<GetQuadrantsOfScreenRectFn>(rebase(kGetQuadrantsOfScreenRectVA));
		SetForDepthSort = reinterpret_cast<SetForDepthSortFn>(rebase(kSetForDepthSortVA));
		gStaticTailTarget = reinterpret_cast<uintptr_t>(rebase(kDrawStaticViewTailVA));
		gDynamicTailTarget = reinterpret_cast<uintptr_t>(rebase(kDrawDynamicViewTailVA));

		gMode = requested;

		if (gMode != Mode::Parallel) workers = 0;
		if (workers > 0) gPool.Start(workers, workerAffinity);

		bool a = InstallDetour(gStaticDetour, kDrawStaticViewVA, kStaticFrameByte,
			reinterpret_cast<void*>(&DetourStaticView));
		bool b = InstallDetour(gDynamicDetour, kDrawDynamicViewVA, kDynamicFrameByte,
			reinterpret_cast<void*>(&DetourDynamicView));
		if (!a && !b)
		{
			if (workers > 0) gPool.Stop();
			return false;
		}

		gInstalled = true;
		const char* name = gMode == Mode::Passthru ? "passthru"
			: gMode == Mode::TailOnly ? "tailonly"
			: gMode == Mode::Serial ? "serial"
			: "parallel";
		Logger::GetInstance().WriteLineFormatted(LogLevel::Info, "parallel cull installed: mode=%s workers=%u affinity=0x%08lX static=%s dynamic=%s",
			name, workers, static_cast<unsigned long>(workerAffinity), a ? "ok" : "skip", b ? "ok" : "skip");
		return true;
	}

	void Uninstall()
	{
		if (!gInstalled) return;
		RemoveDetour(gStaticDetour);
		RemoveDetour(gDynamicDetour);
		gPool.Stop();
		gInstalled = false;
		Logger::GetInstance().WriteLineFormatted(LogLevel::Info, "parallel cull uninstalled");
	}

} // namespace ParallelRenderCull
