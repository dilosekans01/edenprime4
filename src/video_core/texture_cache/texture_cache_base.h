// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <limits>
#include <mutex>
#include <span>
#include <type_traits>
// TODO: find out which don't require stable iters
#include <unordered_map>
#include "common/container/unordered_map.h"
#include "common/container/unordered_set.h"
#include <vector>
#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>
#include <queue>

#include "common/common_types.h"
#include "common/hash.h"
#include "common/literals.h"
#include "common/lru_cache.h"
#include <ranges>
#include "common/scratch_buffer.h"
#include "common/slot_vector.h"
#include "common/thread_worker.h"
#include "video_core/compatible_formats.h"
#include "video_core/control/channel_state_cache.h"
#include "video_core/delayed_destruction_ring.h"
#include "video_core/engines/fermi_2d.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/descriptor_table.h"
#include "video_core/texture_cache/image_base.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/image_view_base.h"
#include "video_core/texture_cache/render_targets.h"
#include "video_core/texture_cache/types.h"
#include "video_core/textures/texture.h"

namespace Tegra {
namespace Control {
struct ChannelState;
}
} // namespace Tegra

namespace VideoCommon {

using Tegra::Texture::TICEntry;
using Tegra::Texture::TSCEntry;
using VideoCore::Surface::PixelFormat;
using namespace Common::Literals;

struct ImageViewInOut {
    u32 index{};
    bool blacklist{};
    ImageViewId id{};
};

struct AsyncDecodeContext {
    ImageId image_id;
    Common::ScratchBuffer<u8> decoded_data;
    boost::container::small_vector<BufferImageCopy, 16> copies;
    std::mutex mutex;
    std::atomic_bool complete;
};

using TextureCacheGPUMap = ::Common::unordered_map<u64, std::vector<ImageId>, Common::IdentityHash<u64>>;

class TextureCacheChannelInfo : public ChannelInfo {
public:
    TextureCacheChannelInfo() = delete;
    TextureCacheChannelInfo(Tegra::Control::ChannelState& state) noexcept;
    TextureCacheChannelInfo(const TextureCacheChannelInfo& state) = delete;
    TextureCacheChannelInfo& operator=(const TextureCacheChannelInfo&) = delete;

    DescriptorTable<TICEntry> graphics_image_table;
    DescriptorTable<TSCEntry> graphics_sampler_table;
    DescriptorTable<TICEntry> compute_image_table;
    DescriptorTable<TSCEntry> compute_sampler_table;

    // TODO: still relies on bad iterators :(
    std::unordered_map<TICEntry, ImageViewId> image_views;
    std::unordered_map<TSCEntry, SamplerId> samplers;

    ::Common::unordered_map<u32, SamplerId> sampler_ids;
    ::Common::unordered_map<u32, ImageViewId> image_view_ids;

    TextureCacheGPUMap* gpu_page_table = nullptr;
    TextureCacheGPUMap* sparse_page_table = nullptr;
};

template <class P>
class TextureCache : public VideoCommon::ChannelSetupCaches<TextureCacheChannelInfo> {
    /// Address shift for caching images into a hash table
    static constexpr u64 YUZU_PAGEBITS = 20;

    /// Enables debugging features to the texture cache
    static constexpr bool ENABLE_VALIDATION = P::ENABLE_VALIDATION;
    /// Implement blits as copies between framebuffers
    static constexpr bool FRAMEBUFFER_BLITS = P::FRAMEBUFFER_BLITS;
    /// True when some copies have to be emulated
    static constexpr bool HAS_EMULATED_COPIES = P::HAS_EMULATED_COPIES;
    /// True when the API can provide info about the memory of the device.
    static constexpr bool HAS_DEVICE_MEMORY_INFO = P::HAS_DEVICE_MEMORY_INFO;
    /// True when the API can do asynchronous texture downloads.
    static constexpr bool IMPLEMENTS_ASYNC_DOWNLOADS = P::IMPLEMENTS_ASYNC_DOWNLOADS;

    static constexpr size_t UNSET_CHANNEL{(std::numeric_limits<size_t>::max)()};

#ifdef YUZU_LEGACY
    static constexpr s64 TARGET_THRESHOLD = 3_GiB;
#else
    static constexpr s64 TARGET_THRESHOLD = 4_GiB;
#endif

    static constexpr s64 DEFAULT_EXPECTED_MEMORY = 1_GiB + 125_MiB;
    static constexpr s64 DEFAULT_CRITICAL_MEMORY = 1_GiB + 625_MiB;
    static constexpr size_t GC_EMERGENCY_COUNTS = 2;

    using Runtime = typename P::Runtime;
    using Image = typename P::Image;
    using ImageAlloc = typename P::ImageAlloc;
    using ImageView = typename P::ImageView;
    using Sampler = typename P::Sampler;
    using Framebuffer = typename P::Framebuffer;
    using AsyncBuffer = typename P::AsyncBuffer;
    using BufferType = typename P::BufferType;

    struct PendingUnswizzle {
        ImageId image_id;
        VideoCommon::ImageInfo info;
        size_t current_offset = 0;
        size_t total_size = 0;
        AsyncBuffer staging_buffer;
        size_t last_submitted_offset = 0;
        size_t bytes_per_slice;
        bool initialized = false;
    };

    struct BlitImages {
        ImageId dst_id;
        ImageId src_id;
        PixelFormat dst_format;
        PixelFormat src_format;
    };

public:
    explicit TextureCache(Runtime&, Tegra::MaxwellDeviceMemoryManager&);

    /// Notify the cache that a new frame has been queued
    void TickFrame();

    /// Return a constant reference to the given image view id
    [[nodiscard]] const ImageView& GetImageView(ImageViewId id) const noexcept;

    /// Return a reference to the given image view id
    [[nodiscard]] ImageView& GetImageView(ImageViewId id) noexcept;

    /// Get the imageview from the graphics descriptor table in the specified index
    [[nodiscard]] ImageView& GetImageView(u32 index) noexcept;

    /// Mark an image as modified from the GPU
    void MarkModification(ImageId id) noexcept;

    /// Fill image_view_ids with the graphics/compute images in indices
    void FillImageViews(std::span<ImageViewInOut> views, bool compute, bool blacklist = true);

    /// Handle feedback loops during draws.
    void CheckFeedbackLoop(std::span<const ImageViewInOut> views);

    /// Get the sampler from the graphics/compute descriptor table in the specified index
    Sampler* GetSampler(u32 index, bool compute);

    /// Get the sampler id from the graphics/compute descriptor table in the specified index
    SamplerId GetSamplerId(u32 index, bool compute);

    /// Return a constant reference to the given sampler id
    [[nodiscard]] const Sampler& GetSampler(SamplerId id) const noexcept;

    /// Return a reference to the given sampler id
    [[nodiscard]] Sampler& GetSampler(SamplerId id) noexcept;

    /// Refresh the state for graphics/compute image view and sampler descriptors
    void SynchronizeDescriptors(bool compute);

    /// Updates the Render Targets if they can be rescaled
    /// @retval True if the Render Targets have been rescaled.
    bool RescaleRenderTargets();

    /// Update bound render targets and upload memory if necessary
    /// @param is_clear True when the render targets are being used for clears
    void UpdateRenderTargets(bool is_clear);

    /// Find a framebuffer with the currently bound render targets
    /// UpdateRenderTargets should be called before this
    Framebuffer* GetFramebuffer();

    /// Mark images in a range as modified from the CPU
    void WriteMemory(DAddr cpu_addr, size_t size);

    /// Download contents of host images to guest memory in a region
    void DownloadMemory(DAddr cpu_addr, size_t size);

    std::optional<VideoCore::RasterizerDownloadArea> GetFlushArea(DAddr cpu_addr, u64 size);

    /// Remove images in a region
    void UnmapMemory(DAddr cpu_addr, size_t size);

    /// Remove images in a region
    void UnmapGPUMemory(size_t as_id, GPUVAddr gpu_addr, size_t size);

    /// Blit an image with the given parameters
    bool BlitImage(const Tegra::Engines::Fermi2D::Surface& dst,
                   const Tegra::Engines::Fermi2D::Surface& src,
                   const Tegra::Engines::Fermi2D::Config& copy);

    /// Try to find a cached image view in the given CPU address
    [[nodiscard]] std::pair<ImageView*, bool> TryFindFramebufferImageView(
        const Tegra::FramebufferConfig& config, DAddr cpu_addr);

    /// Return true when there are uncommitted images to be downloaded
    [[nodiscard]] bool HasUncommittedFlushes() const noexcept;

    /// Return true when the caller should wait for async downloads
    [[nodiscard]] bool ShouldWaitAsyncFlushes() const noexcept;

    /// Commit asynchronous downloads
    void CommitAsyncFlushes();

    /// Pop asynchronous downloads
    void PopAsyncFlushes();

    [[nodiscard]] ImageId DmaImageId(const Tegra::DMA::ImageOperand& operand, bool is_upload);

    /// eden-kledd: diagnostics from the last DmaImageId call
    struct KleddDmaDiag {
        int reason = 0;
        u32 image_index = 0;
        u32 flags = 0;
        u32 width = 0;
        u32 height = 0;
        u32 format = 0;
        u64 tick = 0;
        u32 candidates = 0;
    };
    KleddDmaDiag kledd_last_dma{};

    /// eden-kledd: queue an async readback of a D24S8 region into guest memory with depth and
    /// stencil downloaded separately and re-packed when the download completes.
    bool KleddDownloadDepthStencilToGuest(Image* image, const BufferImageCopy& copy,
                                          GPUVAddr address, size_t size);

private:
    void KleddDownloadDepthStencil(Image& image);

    /// eden-kledd diagnostic: which large images get (re)uploaded, summarised every 2 seconds
    struct KleddBigUpload {
        u32 count = 0;
        u64 bytes = 0;
        ImageInfo info{};
    };
    std::unordered_map<GPUVAddr, KleddBigUpload> kledd_big_uploads;
    std::chrono::steady_clock::time_point kledd_big_upload_log{};
    void KleddNoteBigUpload(const Image& image);

    /// eden-kledd: write watch for large single-level 3D images. After an upload the image stays
    /// tracked; a CPU write unprotects and marks only the block-linear z group it hit, so the next
    /// upload sends just the written groups instead of the whole image.
    struct KleddWatch {
        u64 group_bytes = 0;
        u32 group_slices = 0;
        u32 num_groups = 0;
        /// Every write since the last upload was seen by the watch, so the dirty groups are the
        /// only ones whose host copy is stale; false after the image was untracked
        bool complete = false;
        std::vector<u8> tracked;
        std::vector<u8> dirty;
        u32 dirty_by_remap = 0;
        /// Bytes covered by the remap events and write faults since the last upload (log only)
        u64 remapped_bytes = 0;
        u32 dirty_by_write = 0;
        u32 views_rebuilt = 0;
        /// GPU ranges of the image whose page mapping changed; their map views are rebuilt from
        /// the page table before their groups are protected again
        std::vector<std::pair<GPUVAddr, GPUVAddr>> remapped;
    };
    ::Common::unordered_map<ImageId, KleddWatch> kledd_watches;
    std::chrono::steady_clock::time_point kledd_decline_log{};
    KleddWatch* KleddFindWatch(ImageId image_id);
    /// Calls func(cpu_addr, size) for every CPU page range owned by one z group. The groups
    /// partition the image's pages; sparse images go through their registered map views.
    template <typename Func>
    void KleddForEachGroupRange(ImageId image_id, const ImageBase& image, const KleddWatch& watch,
                                u32 group, Func&& func);
    void KleddTrackGroups(ImageId image_id, const ImageBase& image, KleddWatch& watch, bool track);
    /// Marks the z groups covering image offsets [begin, end) dirty and unprotects them
    void KleddMarkGroups(ImageId image_id, const ImageBase& image, KleddWatch& watch,
                         u64 offset_begin, u64 offset_end);
    void KleddMarkWritten(ImageId image_id, const ImageBase& image, KleddWatch& watch,
                          DAddr cpu_addr, size_t size);
    /// A GPU page of the image was (un)mapped: only its z groups need a new upload
    void KleddMarkRemapped(ImageId image_id, const ImageBase& image, KleddWatch& watch,
                           GPUVAddr gpu_addr, size_t size);
    /// Replaces the map views of the remapped ranges with the current page table mapping
    void KleddRebuildViews(ImageId image_id, const ImageBase& image, KleddWatch& watch);
    /// Frames a watched image may go undrawn before the garbage collector may take it, as long
    /// as texture memory is below the critical budget
    static constexpr u64 KLEDD_WATCH_GC_TICKS = 600;
    struct KleddUploadStats {
        u32 read = 0;        ///< groups read from guest memory and unswizzled
        u32 zero_filled = 0; ///< groups without a mapped page, filled with zeros on the GPU
        u64 staged_bytes = 0;
        u64 unswizzle_us = 0; ///< wall time spent reading and unswizzling on the workers
        u32 threads = 1;
    };
    /// Workers that read and unswizzle the groups of one upload in parallel, with one guest
    /// read buffer per request slot
    std::unique_ptr<Common::ThreadWorker> kledd_upload_workers;
    size_t kledd_upload_worker_count = 0;
    std::vector<Common::ScratchBuffer<u8>> kledd_upload_scratch;
    /// True when any page of the z group's GPU range is mapped (always for non-sparse images)
    bool KleddGroupMapped(const ImageBase& image, const KleddWatch& watch, u32 group);
    /// Uploads the given z groups: mapped ones are read and unswizzled through bounded staging
    /// requests, unmapped ones are filled with zeros from one zeroed staging region
    KleddUploadStats KleddUploadGroups(Image& image, const KleddWatch& watch,
                                       std::span<const u32> groups);
    bool KleddTryDeltaUpload(Image& image, ImageId image_id);

    /// eden-kledd: log the previous frame if it took too long, then reset the frame counters
    void KleddLogLongFrame();
    std::chrono::steady_clock::time_point kledd_last_frame{};

public:

    [[nodiscard]] std::pair<Image*, BufferImageCopy> DmaBufferImageCopy(
        const Tegra::DMA::ImageCopy& copy_info, const Tegra::DMA::BufferOperand& buffer_operand,
        const Tegra::DMA::ImageOperand& image_operand, ImageId image_id, bool modifies_image);

    void DownloadImageIntoBuffer(Image* image, BufferType buffer, size_t buffer_offset,
                                 std::span<const VideoCommon::BufferImageCopy> copies,
                                 GPUVAddr address = 0, size_t size = 0);

    /// Return true when a CPU region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(DAddr addr, size_t size);

    [[nodiscard]] bool IsRescaling() const noexcept;

    [[nodiscard]] bool IsRescaling(const ImageViewBase& image_view) const noexcept;

    /// Create channel state.
    void CreateChannel(Tegra::Control::ChannelState& channel) final override;

    /// Prepare an image to be used
    void PrepareImage(ImageId image_id, bool is_modification, bool invalidate);

    std::recursive_mutex mutex;

private:
    /// Iterate over all page indices in a range
    template <typename Func>
    static void ForEachCPUPage(DAddr addr, size_t size, Func&& func) {
        static constexpr bool RETURNS_BOOL = std::is_same_v<std::invoke_result<Func, u64>, bool>;
        const u64 page_end = (addr + size - 1) >> YUZU_PAGEBITS;
        for (u64 page = addr >> YUZU_PAGEBITS; page <= page_end; ++page) {
            if constexpr (RETURNS_BOOL) {
                if (func(page)) {
                    break;
                }
            } else {
                func(page);
            }
        }
    }

    template <typename Func>
    static void ForEachGPUPage(GPUVAddr addr, size_t size, Func&& func) {
        static constexpr bool RETURNS_BOOL = std::is_same_v<std::invoke_result<Func, u64>, bool>;
        const u64 page_end = (addr + size - 1) >> YUZU_PAGEBITS;
        for (u64 page = addr >> YUZU_PAGEBITS; page <= page_end; ++page) {
            if constexpr (RETURNS_BOOL) {
                if (func(page)) {
                    break;
                }
            } else {
                func(page);
            }
        }
    }

    void OnGPUASRegister(size_t map_id) final override;

    /// Runs the Garbage Collector.
    void RunGarbageCollector();

    /// Find or create an image view in the guest descriptor table
    ImageViewId VisitImageView(u32 index, bool compute);

    /// Find or create a framebuffer with the given render target parameters
    FramebufferId GetFramebufferId(const RenderTargets& key);

    /// Refresh the contents (pixel data) of an image
    [[nodiscard]] bool IsDownloadable(const ImageBase& image) const noexcept;

    void RefreshContents(Image& image, ImageId image_id);

    /// Upload data from guest to an image
    template <typename StagingBuffer>
    void UploadImageContents(Image& image, StagingBuffer& staging_buffer);

    /// Create a new image view from a guest descriptor
    [[nodiscard]] ImageViewId CreateImageView(const TICEntry& config);

    /// Find or create an image from the given parameters
    [[nodiscard]] ImageId FindOrInsertImage(const ImageInfo& info, GPUVAddr gpu_addr,
                                            RelaxedOptions options = RelaxedOptions{});

    /// Find an image from the given parameters
    [[nodiscard]] ImageId FindImage(const ImageInfo& info, GPUVAddr gpu_addr,
                                    RelaxedOptions options);

    /// Create an image from the given parameters
    [[nodiscard]] ImageId InsertImage(const ImageInfo& info, GPUVAddr gpu_addr,
                                      RelaxedOptions options);

    /// Create a new image and join perfectly matching existing images
    /// Remove joined images from the cache
    [[nodiscard]] ImageId JoinImages(const ImageInfo& info, GPUVAddr gpu_addr, DAddr cpu_addr);

    [[nodiscard]] ImageId FindDMAImage(const ImageInfo& info, GPUVAddr gpu_addr);

    /// Return a blit image pair from the given guest blit parameters
    [[nodiscard]] std::optional<BlitImages> GetBlitImages(
        const Tegra::Engines::Fermi2D::Surface& dst, const Tegra::Engines::Fermi2D::Surface& src,
        const Tegra::Engines::Fermi2D::Config& copy);

    /// Find or create a sampler from a guest descriptor sampler
    [[nodiscard]] SamplerId FindSampler(const TSCEntry& config, bool compute);

    /// Find or create an image view for the given color buffer index
    [[nodiscard]] ImageViewId FindColorBuffer(size_t index);

    /// Find or create an image view for the depth buffer
    [[nodiscard]] ImageViewId FindDepthBuffer();

    /// Find or create a view for a render target with the given image parameters
    [[nodiscard]] ImageViewId FindRenderTargetView(const ImageInfo& info, GPUVAddr gpu_addr);

    /// Iterates over all the images in a region calling func
    template <typename Func>
    void ForEachImageInRegion(DAddr cpu_addr, size_t size, Func&& func);

    template <typename Func>
    void ForEachImageInRegionGPU(size_t as_id, GPUVAddr gpu_addr, size_t size, Func&& func);

    template <typename Func>
    void ForEachSparseImageInRegion(size_t as_id, GPUVAddr gpu_addr, size_t size, Func&& func);

    /// Iterates over all the images in a region calling func
    template <typename Func>
    void ForEachSparseSegment(ImageBase& image, Func&& func);

    /// Find or create an image view in the given image with the passed parameters
    [[nodiscard]] ImageViewId FindOrEmplaceImageView(ImageId image_id, const ImageViewInfo& info);

    /// Register image in the page table
    void RegisterImage(ImageId image);

    /// Unregister image from the page table
    void UnregisterImage(ImageId image);

    /// Track CPU reads and writes for image
    void TrackImage(ImageBase& image, ImageId image_id);

    /// Stop tracking CPU reads and writes for image
    void UntrackImage(ImageBase& image, ImageId image_id);

    /// Delete image from the cache
    void DeleteImage(ImageId image, bool immediate_delete = false);

    /// Remove image views references from the cache
    void RemoveImageViewReferences(std::span<const ImageViewId> removed_views);

    /// Remove framebuffers using the given image views from the cache
    void RemoveFramebuffers(std::span<const ImageViewId> removed_views);

    /// Mark an image as modified from the GPU
    void MarkModification(ImageBase& image) noexcept;

    /// Synchronize image aliases, copying data if needed
    void SynchronizeAliases(ImageId image_id);

    /// Prepare an image view to be used
    void PrepareImageView(ImageViewId image_view_id, bool is_modification, bool invalidate);

    /// Execute copies from one image to the other, even if they are incompatible
    void CopyImage(ImageId dst_id, ImageId src_id, std::vector<ImageCopy> copies);

    /// Bind an image view as render target, downloading resources preemtively if needed
    void BindRenderTarget(ImageViewId* old_id, ImageViewId new_id);

    /// Create a render target from a given image and image view parameters
    [[nodiscard]] std::pair<FramebufferId, ImageViewId> RenderTargetFromImage(
        ImageId, const ImageViewInfo& view_info);

    /// Returns true if the current clear parameters clear the whole image of a given image view
    [[nodiscard]] bool IsFullClear(ImageViewId id);

    [[nodiscard]] std::pair<u32, u32> PrepareDmaImage(ImageId dst_id, GPUVAddr base_addr,
                                                      bool mark_as_modified);

    bool ImageCanRescale(ImageBase& image);
    void InvalidateScale(Image& image);
    bool ScaleUp(Image& image);
    bool ScaleDown(Image& image);
    u64 GetScaledImageSizeBytes(const ImageBase& image);

    void QueueAsyncDecode(Image& image, ImageId image_id);
    void TickAsyncDecode();

    void QueueAsyncUnswizzle(Image& image, ImageId image_id);
    void TickAsyncUnswizzle();

    Runtime& runtime;

    Tegra::MaxwellDeviceMemoryManager& device_memory;
    std::deque<TextureCacheGPUMap> gpu_page_table_storage;

    RenderTargets render_targets;
    u64 render_targets_serial = 0;
    u32 rt_active_mask = 0;
    std::array<ImageId, 8> rt_image_id{};
    ImageId rt_depth_image_id{};
    u64 texture_bindings_serial = 0;
    u64 last_feedback_loop_serial = 0;
    u64 last_feedback_texture_serial = 0;
    bool last_feedback_loop_result = false;
    FramebufferId last_framebuffer_id{};
    u64 last_framebuffer_serial = 0;

    ::Common::unordered_map<RenderTargets, FramebufferId> framebuffers;
    ::Common::unordered_map<u64, std::vector<ImageMapId>, Common::IdentityHash<u64>> page_table;
    ::Common::unordered_map<ImageId, boost::container::small_vector<ImageViewId, 16>> sparse_views;

    DAddr virtual_invalid_space{};

    bool has_deleted_images = false;
    bool is_rescaling = false;
    u64 total_used_memory = 0;
    u64 minimum_memory;
    u64 expected_memory;
    u64 critical_memory;
    size_t gpu_unswizzle_maxsize = 0;
    size_t swizzle_chunk_size = 0;
    u32 swizzle_slices_per_batch = 0;

    struct BufferDownload {
        GPUVAddr address;
        size_t size;
        // eden-kledd: split depth/stencil readback that must be re-packed before writing
        bool kledd_ds = false;
        bool kledd_stencil_low = false;
        bool kledd_depth_float = false;
        size_t kledd_split = 0;
        u32 kledd_width = 0;
        u32 kledd_height = 0;
        u32 kledd_row = 0;
    };

    struct PendingDownload {
        bool is_swizzle;
        size_t async_buffer_id;
        Common::SlotId object_id;
    };

    Common::SlotVector<Image> slot_images;
    Common::SlotVector<ImageMapView> slot_map_views;
    Common::SlotVector<ImageView> slot_image_views;
    Common::SlotVector<ImageAlloc> slot_image_allocs;
    Common::SlotVector<Sampler> slot_samplers;
    Common::SlotVector<Framebuffer> slot_framebuffers;
    Common::SlotVector<BufferDownload> slot_buffer_downloads;

    // TODO: This data structure is not optimal and it should be reworked

    std::vector<PendingDownload> uncommitted_downloads;
    std::deque<std::vector<PendingDownload>> committed_downloads;
    std::vector<AsyncBuffer> uncommitted_async_buffers;
    std::deque<std::vector<AsyncBuffer>> async_buffers;
    std::deque<AsyncBuffer> async_buffers_death_ring;

    struct LRUItemParams {
        using ObjectType = ImageId;
        using TickType = u64;
    };
    Common::LeastRecentlyUsedCache<LRUItemParams> lru_cache;

 #ifdef YUZU_LEGACY
    static constexpr size_t TICKS_TO_DESTROY = 6;
 #else
    static constexpr size_t TICKS_TO_DESTROY = 8;
#endif
    DelayedDestructionRing<Image, TICKS_TO_DESTROY> sentenced_images;
    DelayedDestructionRing<ImageView, TICKS_TO_DESTROY> sentenced_image_view;
    DelayedDestructionRing<Framebuffer, TICKS_TO_DESTROY> sentenced_framebuffers;

    ::Common::unordered_map<GPUVAddr, ImageAllocId> image_allocs_table;

    Common::ScratchBuffer<u8> swizzle_data_buffer;
    Common::ScratchBuffer<u8> unswizzle_data_buffer;

    u64 modification_tick = 0;
    u64 frame_tick = 0;

    Common::ThreadWorker texture_decode_worker{1, "TextureDecoder", {},
                                               Common::ThreadPlacement::Efficiency};
    std::vector<std::unique_ptr<AsyncDecodeContext>> async_decodes;

    std::deque<PendingUnswizzle> unswizzle_queue;
    u8 current_unswizzle_frame;

    // Join caching
    boost::container::small_vector<ImageId, 4> join_overlap_ids;
    ::Common::unordered_set<ImageId> join_overlaps_found;
    boost::container::small_vector<ImageId, 4> join_left_aliased_ids;
    boost::container::small_vector<ImageId, 4> join_right_aliased_ids;
    ::Common::unordered_set<ImageId> join_ignore_textures;
    boost::container::small_vector<ImageId, 4> join_bad_overlap_ids;
    struct JoinCopy {
        bool is_alias;
        ImageId id;
    };
    boost::container::small_vector<JoinCopy, 4> join_copies_to_do;
    ::Common::unordered_map<ImageId, size_t> join_alias_indices;
};

} // namespace VideoCommon
