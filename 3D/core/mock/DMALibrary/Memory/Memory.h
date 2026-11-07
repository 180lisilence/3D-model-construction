#pragma once

// ============================================================================
// mock/DMALibrary/Memory/Memory.h
//
// 这是一个「影子头文件」。项目里所有文件都是这样引用的：
//     #include <DMALibrary/Memory/Memory.h>
// 尖括号 + 编译期 /I 搜索路径，所以只要把 src\mock 放在 ext 之前，
// 编译器就会优先命中本文件，而不是 ext\DMALibrary\Memory\Memory.h。
//
// 效果：
//   - Physx.h / LineTrace.h / 任何用到 mem 的地方，一行都不用改
//   - 完全没有 vmm.dll / leechcore.dll / FTD3XX.dll 依赖
//   - mem.Read<T> / mem.CreateScatterHandle / mem.AddScatterReadRequest /
//     mem.ExecuteReadScatter 全部落到 mockmem::Space() 这块模拟地址空间上
//
// 语义对齐：
//   - Scatter 句柄真的会「攒」请求，Execute 时统一落地再清空，
//     与 VMMDLL 的行为一致（这样调用方对 scatter 生命周期的假设不会被破坏）
//   - 读到未映射地址返回 false，缓冲区补 0（DMA 读无效页也是这个表现）
// ============================================================================

#include "../../MockMemory.h"

#include <cstdint>
#include <vector>

// 真实的 Memory.h 从 vmmdll.h 拿到这个类型；这里只需要是个不透明句柄
typedef void* VMMDLL_SCATTER_HANDLE;

namespace mockmem
{
    // 全局唯一的模拟地址空间。MockScene 往里写，Memory 从里读。
    inline Memory& Space()
    {
        static Memory space;
        return space;
    }
}

class Memory
{
private:
    struct Request
    {
        uint64_t address;
        void* buffer;
        size_t size;
    };

    struct Handle
    {
        std::vector<Request> requests;
    };

public:
    // ---- 单次读 ----
    bool Read(uintptr_t address, void* buffer, size_t size) const
    {
        return mockmem::Space().Read(address, buffer, size);
    }

    bool Read(uintptr_t address, void* buffer, size_t size, int /*pid*/) const
    {
        return mockmem::Space().Read(address, buffer, size);
    }

    template <typename T>
    T Read(uint64_t address) const
    {
        return mockmem::Space().Read<T>(address);
    }

    template <typename T>
    T Read(void* address) const
    {
        return mockmem::Space().Read<T>(reinterpret_cast<uint64_t>(address));
    }

    uint64_t ReadChain(uint64_t base, const std::vector<uint64_t>& offsets) const
    {
        return mockmem::Space().ReadChain(base, offsets);
    }

    // ---- 散列读 ----
    VMMDLL_SCATTER_HANDLE CreateScatterHandle() const
    {
        return new Handle();
    }

    VMMDLL_SCATTER_HANDLE CreateScatterHandle(int /*pid*/) const
    {
        return new Handle();
    }

    void CloseScatterHandle(VMMDLL_SCATTER_HANDLE handle)
    {
        delete static_cast<Handle*>(handle);
    }

    void AddScatterReadRequest(VMMDLL_SCATTER_HANDLE handle, uint64_t address, void* buffer, size_t size)
    {
        if (!handle || !buffer || size == 0) return;
        static_cast<Handle*>(handle)->requests.push_back(Request{ address, buffer, size });
    }

    template <typename T>
    void AddScatterReadRequest(VMMDLL_SCATTER_HANDLE handle, uint64_t address, T* buffer)
    {
        AddScatterReadRequest(handle, address, reinterpret_cast<void*>(buffer), sizeof(T));
    }

    void ExecuteReadScatter(VMMDLL_SCATTER_HANDLE handle, int /*pid*/ = 0)
    {
        if (!handle) return;
        Handle* h = static_cast<Handle*>(handle);
        for (const auto& r : h->requests)
            mockmem::Space().Read(r.address, r.buffer, r.size);
        h->requests.clear();
    }

    // 项目里没有用到写散列，但保留同名接口避免将来缺符号
    void ExecuteWriteScatter(VMMDLL_SCATTER_HANDLE handle, int /*pid*/ = 0)
    {
        if (!handle) return;
        static_cast<Handle*>(handle)->requests.clear();
    }
};

// 与真实头文件一样，提供一个全局实例
inline Memory mem;
