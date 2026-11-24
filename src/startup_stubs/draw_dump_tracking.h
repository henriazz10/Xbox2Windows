#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace x2w_startup {

struct DrawDump {
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows{};
    std::uint64_t key{};
    UINT count{};
    DXGI_FORMAT format{};
    UINT index{};
};

// A recorded copy belongs to a command-list recording until it is either reset
// or submitted. After submission the queue fence owns the completion decision.
class DrawReadbacks {
    struct Entry {
        ID3D12CommandList* recording;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        DrawDump dump;
    };
    static constexpr unsigned limit = 48;
    std::mutex mutex_;
    std::vector<Entry> entries_;
    unsigned consumed_ = 0;
    unsigned sequence_ = 0;

public:
    bool full()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size() + consumed_ >= limit;
    }

    bool record(ID3D12CommandList* list, DrawDump dump)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (entries_.size() + consumed_ >= limit) return false;
        dump.index = ++sequence_;
        entries_.push_back({list, {}, std::move(dump)});
        return true;
    }

    void discard(ID3D12CommandList* list)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
            [list](const Entry& e) { return e.recording == list; }), entries_.end());
    }

    HRESULT execute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto in_batch = [&](const Entry& entry) {
            return entry.recording && std::find(lists, lists + count, entry.recording) != lists + count;
        };
        queue->ExecuteCommandLists(count, lists);
        if (std::none_of(entries_.begin(), entries_.end(), in_batch)) return S_OK;

        Microsoft::WRL::ComPtr<ID3D12Device> device;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        HRESULT hr = queue->GetDevice(IID_PPV_ARGS(&device));
        if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (SUCCEEDED(hr)) hr = queue->Signal(fence.Get(), 1);
        for (auto& entry : entries_) {
            if (!in_batch(entry)) continue;
            entry.recording = nullptr;
            // A signalling failure provides no safe point to read or release
            // submitted storage. Retain it within the same capture budget.
            if (SUCCEEDED(hr)) entry.fence = fence;
        }
        return hr;
    }

    std::vector<DrawDump> take_completed()
    {
        std::vector<DrawDump> complete;
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = entries_.begin(); it != entries_.end();) {
            const UINT64 value = it->fence ? it->fence->GetCompletedValue() : 0;
            if (!value) {
                ++it;
                continue;
            }
            // D3D12 reports UINT64_MAX for device removal, including during a wait.
            if (value != UINT64_MAX) {
                complete.push_back(std::move(it->dump));
                ++consumed_;
            }
            it = entries_.erase(it);
        }
        return complete;
    }
};
}  // namespace x2w_startup
