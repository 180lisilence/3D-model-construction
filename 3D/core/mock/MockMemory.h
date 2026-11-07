#pragma once

// ============================================================================
// mock/MockMemory.h
//
// 模拟内存后端。接口与 DMALibrary 的 Memory 类保持一致（Read / Write /
// ReadChain / Scatter），这样 Physx.h 里的 mem.Read<T>(addr) 调用点
// 一行都不用改。
//
// 地址空间：一个 std::map<addr, vector<uint8_t>> 的稀疏页表。分配方式模仿
// 真实进程内存——每块分配有固定基址、互不重叠、地址带随机间隙，这样
// "指针链读错地址" 这种 bug 会以读到 0 的形式暴露出来，而不是恰好读到
// 相邻对象的字段（真实内存布局里相邻字段极易造成假阳性）。
//
// 用法：
//   mock::Memory m;
//   uint64_t p = m.Alloc(sizeof(ShapeT));
//   m.Write(p, &shape, sizeof(shape));
//   ShapeT got = m.Read<ShapeT>(p);      // 与 mem.Read<T> 同签名
// ============================================================================

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <map>
#include <vector>
#include <string>
#include <stdexcept>

namespace mockmem
{
    // 分配粒度：与真实堆页对齐，1MB 一页
    static constexpr uint64_t kPageSize = 0x100000;

    class Memory
    {
    public:
        Memory()
        {
            // 模拟 64 位用户态地址空间起点，留足间隙让野指针明显出错
            next_ = 0x000001E4'00000000ull;
            allocated_bytes_ = 0;
        }

        // ---- 分配 ----
        uint64_t Alloc(size_t bytes, const char* tag = nullptr)
        {
            if (bytes == 0) bytes = 1;

            uint64_t base = next_;
            size_t pages = (bytes + kPageSize - 1) / kPageSize;
            next_ += pages * kPageSize;

            Block b;
            b.base = base;
            b.size = bytes;
            b.data.assign(bytes, 0);
            if (tag) b.tag = tag;
            blocks_[base] = std::move(b);

            pages_used_ += pages;
            allocated_bytes_ += bytes;
            return base;
        }

        // 分配并立即写入
        template <typename T>
        uint64_t AllocObject(const T& value, const char* tag = nullptr)
        {
            uint64_t p = Alloc(sizeof(T), tag);
            Write(p, &value, sizeof(T));
            return p;
        }

        // 释放（仅用于模拟 shape 被销毁）
        void Free(uint64_t addr)
        {
            auto it = blocks_.find(addr);
            if (it == blocks_.end()) return;
            allocated_bytes_ -= it->second.size;
            blocks_.erase(it);
        }

        // ---- 读写 ----
        bool Read(uint64_t addr, void* buffer, size_t size) const
        {
            if (!buffer || size == 0) return false;
            std::memset(buffer, 0, size);

            uint64_t cur = addr;
            size_t remain = size;
            uint8_t* out = reinterpret_cast<uint8_t*>(buffer);

            while (remain > 0)
            {
                auto it = FindBlock(cur);
                if (it == blocks_.end())
                {
                    // 未映射：补 0，与 DMA 读无效地址的行为一致
                    return false;
                }

                const Block& b = it->second;
                if (cur < b.base) return false;

                size_t off = static_cast<size_t>(cur - b.base);
                if (off >= b.size)
                {
                    // 落到分配块之后的空洞
                    return false;
                }

                size_t avail = b.size - off;
                size_t take = avail < remain ? avail : remain;
                std::memcpy(out, b.data.data() + off, take);

                out += take;
                cur += take;
                remain -= take;

                if (remain > 0)
                {
                    // 跨块：跳到下一块的起点，中间空洞补 0
                    cur = NextBlockBase(cur);
                    if (cur == 0) return false;
                }
            }
            return true;
        }

        template <typename T>
        T Read(uint64_t addr) const
        {
            T v{};
            std::memset(&v, 0, sizeof(T));
            Read(addr, &v, sizeof(T));
            return v;
        }

        bool Write(uint64_t addr, const void* buffer, size_t size)
        {
            if (!buffer || size == 0) return false;

            uint64_t cur = addr;
            size_t remain = size;
            const uint8_t* in = reinterpret_cast<const uint8_t*>(buffer);

            while (remain > 0)
            {
                auto it = FindBlock(cur);
                if (it == blocks_.end()) return false;

                Block& b = it->second;
                if (cur < b.base) return false;

                size_t off = static_cast<size_t>(cur - b.base);
                if (off >= b.size)
                {
                    // 越界写：扩展该块（模拟堆上相邻分配）
                    size_t grow = off + remain;
                    b.data.resize(grow, 0);
                    b.size = grow;
                }

                size_t avail = b.size - off;
                size_t take = avail < remain ? avail : remain;
                std::memcpy(b.data.data() + off, in, take);

                in += take;
                cur += take;
                remain -= take;

                if (remain > 0)
                {
                    cur = NextBlockBase(cur);
                    if (cur == 0) return false;
                }
            }
            return true;
        }

        template <typename T>
        bool Write(uint64_t addr, const T& value)
        {
            return Write(addr, &value, sizeof(T));
        }

        // ---- 指针链 ----
        uint64_t ReadChain(uint64_t base, const std::vector<uint64_t>& offsets) const
        {
            if (offsets.empty()) return 0;
            uint64_t r = Read<uint64_t>(base + offsets[0]);
            for (size_t i = 1; i < offsets.size(); i++)
                r = Read<uint64_t>(r + offsets[i]);
            return r;
        }

        // ---- 统计（供界面显示）----
        size_t AllocatedBytes() const { return allocated_bytes_; }
        size_t BlockCount() const { return blocks_.size(); }
        size_t PagesUsed() const { return pages_used_; }

        // 调试：打印全部块，用于核对布局
        void DumpBlocks() const
        {
            std::printf("%-18s %-10s %-8s %s\n", "BASE", "SIZE", "PAGES", "TAG");
            for (const auto& [base, b] : blocks_)
            {
                std::printf("0x%016llX %-10zu %-8zu %s\n",
                    (unsigned long long)base, b.size,
                    (b.size + kPageSize - 1) / kPageSize,
                    b.tag.empty() ? "-" : b.tag.c_str());
            }
            std::printf("total: %zu bytes in %zu blocks, %zu pages\n",
                allocated_bytes_, blocks_.size(), pages_used_);
        }

        // 按 tag 查找块基址（场景构建时用来定位）
        uint64_t FindByTag(const std::string& tag) const
        {
            for (const auto& [base, b] : blocks_)
                if (b.tag == tag) return base;
            return 0;
        }

    private:
        struct Block
        {
            uint64_t base = 0;
            size_t size = 0;
            std::string tag;
            std::vector<uint8_t> data;
        };

        std::map<uint64_t, Block> blocks_;
        uint64_t next_ = 0;
        size_t allocated_bytes_ = 0;
        size_t pages_used_ = 0;

        std::map<uint64_t, Block>::const_iterator FindBlock(uint64_t addr) const
        {
            auto it = blocks_.upper_bound(addr);
            if (it == blocks_.begin()) return blocks_.end();
            --it;
            if (addr < it->second.base) return blocks_.end();
            return it;
        }

        std::map<uint64_t, Block>::iterator FindBlock(uint64_t addr)
        {
            auto it = blocks_.upper_bound(addr);
            if (it == blocks_.begin()) return blocks_.end();
            --it;
            if (addr < it->second.base) return blocks_.end();
            return it;
        }

        uint64_t NextBlockBase(uint64_t after) const
        {
            auto it = blocks_.upper_bound(after);
            if (it == blocks_.end()) return 0;
            return it->second.base;
        }
    };
} // namespace mockmem
