// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <atomic>
#include <memory>
#include <span>
#include <vector>

#include "audio_core/audio_core.h"
#include "audio_core/common/common.h"
#include "audio_core/sink/sink_stream.h"
#include "common/common_types.h"
#include "common/fixed_point.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/perf_stats.h"

namespace AudioCore::Sink {

void SinkStream::AppendBuffer(SinkBuffer& buffer, std::span<s16> samples) {
    if (type == StreamType::In)
        return;

    constexpr s32 min = (std::numeric_limits<s16>::min)();
    constexpr s32 max = (std::numeric_limits<s16>::max)();

    auto yuzu_volume = Settings::Volume();
    if (yuzu_volume > 1.0f)
        yuzu_volume = 0.6f + 20.0f * std::log10(yuzu_volume);
    auto const volume = system_volume * device_volume * yuzu_volume;

    if (system_channels > device_channels) {
        static constexpr std::array<f32, 4> tcoeff{1.0f, 0.596f, 0.354f, 0.707f};
        for (u32 r_offs = 0, w_offs = 0; r_offs < samples.size(); r_offs += system_channels, w_offs += device_channels) {
            std::array<f32, 6> ccoeff{0.f};
            for (u32 i = 0; i < system_channels; ++i)
                ccoeff[i] = f32(samples[r_offs + i]);

            std::array<f32, 6> rcoeff{
                ccoeff[u32(Channels::FrontLeft)],
                ccoeff[u32(Channels::BackLeft)],
                ccoeff[u32(Channels::Center)],
                ccoeff[u32(Channels::LFE)],
                ccoeff[u32(Channels::BackRight)],
                ccoeff[u32(Channels::FrontRight)],
            };

            const f32 left  = rcoeff[0] * tcoeff[0] + rcoeff[2] * tcoeff[1] + rcoeff[3] * tcoeff[2] + rcoeff[1] * tcoeff[3];
            const f32 right = rcoeff[5] * tcoeff[0] + rcoeff[2] * tcoeff[1] + rcoeff[3] * tcoeff[2] + rcoeff[4] * tcoeff[3];

            samples[w_offs + 0] = s16(std::clamp(s32(left * volume),  min, max));
            samples[w_offs + 1] = s16(std::clamp(s32(right * volume), min, max));
        }

        PushAndQueue(buffer, samples.subspan(0, samples.size() / system_channels * device_channels));
    } else if (system_channels < device_channels) {
        std::vector<s16> new_samples(samples.size() / system_channels * device_channels);
        for (u32 r_offs = 0, w_offs = 0; r_offs < samples.size(); r_offs += system_channels, w_offs += device_channels)
            for (u32 channel = 0; channel < system_channels; ++channel)
                new_samples[w_offs + channel] = s16(std::clamp(s32(f32(samples[r_offs + channel]) * volume), min, max));

        PushAndQueue(buffer, new_samples);
    } else {
        if (volume != 1.0f) {
            for (u32 i = 0; i < samples.size(); ++i)
                samples[i] = s16(std::clamp(s32(f32(samples[i]) * volume), min, max));
        }

        PushAndQueue(buffer, samples);
    }
}

void SinkStream::PushAndQueue(SinkBuffer& buffer, std::span<const s16> samples) {
    // Push the samples before the buffer entry, so the consumer can never dequeue an entry whose
    // samples are not in the ring yet, and account for anything the ring could not take.
    const size_t pushed = samples_buffer.Push(samples);
    if (pushed < samples.size()) {
        NoteIssue(SinkIssue::RingFull, (samples.size() - pushed) / device_channels);
    }
    queue.EmplaceWait(buffer);
    ++queued_buffers;
}

std::vector<s16> SinkStream::ReleaseBuffer(u64 num_samples) {
    auto samples{samples_buffer.Pop(num_samples)};

    // TODO: Up-mix to 6 channels if the game expects it.
    // For audio input this is unlikely to ever be the case though.

    // Incoming mic volume seems to always be very quiet, so multiply by an additional 8 here.
    // TODO: Play with this and find something that works better.
    constexpr s32 min = (std::numeric_limits<s16>::min)();
    constexpr s32 max = (std::numeric_limits<s16>::max)();
    auto volume{system_volume * device_volume * 8};
    for (u32 i = 0; i < samples.size(); i++)
        samples[i] = s16(std::clamp(s32(f32(samples[i]) * volume), min, max));

    if (samples.size() < num_samples)
        samples.resize(num_samples, 0);
    return samples;
}

void SinkStream::ClearQueue() {
    std::scoped_lock lk{release_mutex};

    samples_buffer.Pop();
    SinkBuffer tmp;
    while (queue.TryPop(tmp));

    queued_buffers = 0;
    playing_buffer = {};
    playing_buffer.consumed = true;
}

void SinkStream::ProcessAudioIn(std::span<const s16> input_buffer, std::size_t num_frames) {
    const std::size_t num_channels = GetDeviceChannels();
    const std::size_t frame_size = num_channels;
    const std::size_t frame_size_bytes = frame_size * sizeof(s16);
    size_t frames_written{0};

    // If we're paused or going to shut down, we don't want to consume buffers as coretiming is
    // paused and we'll desync, so just return. The same applies while the guest is stalled.
    if (system.IsPaused() || system.IsShuttingDown() || system.IsApplicationStalled())
        return;

    while (frames_written < num_frames) {
        // If the playing buffer has been consumed or has no frames, we need a new one
        if (playing_buffer.consumed || playing_buffer.frames == 0) {
            if (!queue.TryPop(playing_buffer)) {
                // If no buffer was available we've underrun, just push the samples and
                // continue.
                samples_buffer.Push(&input_buffer[frames_written * frame_size], (num_frames - frames_written) * frame_size);
                frames_written = num_frames;
                continue;
            }
            // Successfully dequeued a new buffer.
            queued_buffers--;
        }

        // Get the minimum frames available between the currently playing buffer, and the
        // amount we have left to fill
        size_t frames_available{std::min<u64>(playing_buffer.frames - playing_buffer.frames_played, num_frames - frames_written)};

        samples_buffer.Push(&input_buffer[frames_written * frame_size], frames_available * frame_size);

        frames_written += frames_available;
        playing_buffer.frames_played += frames_available;

        // If that's all the frames in the current buffer, add its samples and mark it as
        // consumed
        if (playing_buffer.frames_played >= playing_buffer.frames)
            playing_buffer.consumed = true;
    }

    if (frames_written > 0) {
        std::memcpy(&last_frame[0], &input_buffer[(frames_written - 1) * frame_size], frame_size_bytes);
    }

    // update sample counts fuer audio-ins
    {
        std::scoped_lock lk{sample_count_lock};
        last_sample_count_update_time = system.CoreTiming().GetGlobalTimeNs();
        min_played_sample_count = max_played_sample_count;
        max_played_sample_count += frames_written;
    }
}

void SinkStream::ProcessAudioOutAndRender(std::span<s16> output_buffer, std::size_t num_frames) {
    const std::size_t num_channels = GetDeviceChannels();
    const std::size_t frame_size = num_channels;
    const std::size_t frame_size_bytes = frame_size * sizeof(s16);
    size_t frames_written{0};
    size_t actual_frames_written{0};

    {
        // Diagnostic: a long gap between host callbacks means the backend thread was starved
        // or the stream stalled, in which case the device replays whatever it last had.
        const auto now = std::chrono::steady_clock::now();
        if (last_callback_time.time_since_epoch().count() != 0) {
            const auto gap_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    now - last_callback_time)
                                    .count();
            if (gap_ms >= 50) {
                NoteIssue(SinkIssue::CallbackGap, static_cast<u64>(gap_ms));
            }
        }
        last_callback_time = now;
    }

    if (system.IsApplicationStalled()) {
        // The guest is suspended until the GPU catches up, so its time stands still. Hold the
        // last played frame and consume nothing, like a pause: the render thread then blocks on
        // the full queue and the renderer resumes from the same point afterwards, instead of
        // running ahead of the game and looping whatever buffer it could not refill.
        for (size_t i = 0; i < num_frames; i++)
            std::memcpy(&output_buffer[i * frame_size], last_frame.data(), frame_size_bytes);
        return;
    }

    if (system.IsPaused() || system.IsShuttingDown()) {
        if (system.IsShuttingDown()) {
            std::scoped_lock lk{release_mutex};
            queued_buffers.store(0);
            release_cv.notify_one();
        }
        for (size_t i = 0; i < num_frames; i++)
            std::memset(&output_buffer[i * frame_size], 0, frame_size_bytes);
        return;
    }

    if (type != StreamType::Render) {
        actual_frames_written = PopFrames(output_buffer.data(), num_frames);
        frames_written = num_frames;
    } else {
        // eden-kledd: play the game's sound at the game's pace. When the game renders fewer
        // frames than usual, its cutscene timeline moves slower than real time, and sound
        // played at full speed runs ahead of the lips. Consuming the queue at the same rate
        // also holds back the DSP (it waits for free space), so the voices advance in step.
        // The time stretch keeps the pitch; at full speed it passes the sound through exactly.
        const double rate = UpdateGamePace();
        actual_frames_written = pace_stretch.Process(
            output_buffer.data(), num_frames, frame_size, rate,
            [this](s16* dst, size_t frames) { return PopFrames(dst, frames); });
        frames_written = num_frames;
    }

    std::memcpy(last_frame.data(), &output_buffer[(frames_written - 1) * frame_size], frame_size_bytes);

    {
        std::scoped_lock lk{sample_count_lock};
        last_sample_count_update_time = system.CoreTiming().GetGlobalTimeNs();
        min_played_sample_count = max_played_sample_count;
        max_played_sample_count += actual_frames_written;
    }
}

size_t SinkStream::PopFrames(s16* dst, size_t frames) {
    const size_t frame_size = GetDeviceChannels();
    const size_t frame_size_bytes = frame_size * sizeof(s16);
    size_t written{0};
    size_t actual{0};
    while (written < frames) {
        if (playing_buffer.consumed || playing_buffer.frames == 0) {
            std::unique_lock lk{release_mutex};

            if (!queue.TryPop(playing_buffer)) {
                lk.unlock();
                const s16* hold = written > 0 ? &dst[(written - 1) * frame_size] : last_frame.data();
                for (size_t i = written; i < frames; i++)
                    std::memcpy(&dst[i * frame_size], hold, frame_size_bytes);
                NoteIssue(SinkIssue::Underrun, frames - written);
                written = frames;
                continue;
            }

            --queued_buffers;
            lk.unlock();
            release_cv.notify_one();
        }

        const size_t frames_available =
            std::min<u64>(playing_buffer.frames - playing_buffer.frames_played, frames - written);

        const size_t wanted_samples = frames_available * frame_size;
        const size_t popped_samples = samples_buffer.Pop(&dst[written * frame_size], wanted_samples);
        if (popped_samples < wanted_samples) {
            // The queue promised more samples than the ring holds. Hold the last good frame
            // instead of leaving whatever the backend had in this buffer, which audibly loops.
            const size_t popped_frames = popped_samples / frame_size;
            const s16* hold = (written + popped_frames) > 0
                                  ? &dst[(written + popped_frames - 1) * frame_size]
                                  : last_frame.data();
            for (size_t i = written + popped_frames; i < written + frames_available; i++) {
                std::memcpy(&dst[i * frame_size], hold, frame_size_bytes);
            }
            NoteIssue(SinkIssue::ShortPop, frames_available - popped_frames);
        }

        written += frames_available;
        actual += frames_available;
        playing_buffer.frames_played += frames_available;

        if (playing_buffer.frames_played >= playing_buffer.frames)
            playing_buffer.consumed = true;
    }
    return actual;
}

double SinkStream::UpdateGamePace() {
    const s64 now = std::chrono::steady_clock::now().time_since_epoch().count();
    if (now - pace_last_sample_ns < 250'000'000) {
        return pace_rate;
    }
    pace_last_sample_ns = now;
    const u64 frames = system.GetPerfStats().GetTotalGameFrames();

    // Keep the last 1 s of (time, frame count) samples.
    if (pace_sample_count == pace_samples.size()) {
        std::move(pace_samples.begin() + 1, pace_samples.end(), pace_samples.begin());
        pace_sample_count--;
    }
    pace_samples[pace_sample_count++] = {now, frames};
    if (pace_sample_count < pace_samples.size()) {
        return pace_rate;
    }
    const auto [t0, f0] = pace_samples.front();
    const double fps = static_cast<double>(frames - f0) * 1e9 / static_cast<double>(now - t0);

    // The usual rate: 60 if the game reached it in the last 10 s, else 30. A steady 30 is
    // taken as a scene meant to run at 30, not a slow 60.
    if (pace_fps_count == pace_fps_history.size()) {
        std::move(pace_fps_history.begin() + 1, pace_fps_history.end(), pace_fps_history.begin());
        pace_fps_count--;
    }
    pace_fps_history[pace_fps_count++] = fps;
    double recent_max{0.0};
    for (size_t i = 0; i < pace_fps_count; i++) {
        recent_max = (std::max)(recent_max, pace_fps_history[i]);
    }
    bool steady_30{pace_fps_count >= 6};
    for (size_t i = pace_fps_count >= 6 ? pace_fps_count - 6 : 0; i < pace_fps_count; i++) {
        steady_30 = steady_30 && pace_fps_history[i] > 28.0 && pace_fps_history[i] < 32.5;
    }
    pace_target_fps = (recent_max >= 50.0 && !steady_30) ? 60.0 : 30.0;

    // Less than 3 % under the usual rate plays at full speed; slower is followed down to 70 %.
    // No new frames at all (a loading screen or a pause) keeps the last rate.
    double target_rate = fps <= 1.0 ? pace_rate : std::clamp(fps / pace_target_fps, 0.70, 1.0);
    if (target_rate > 0.97) {
        target_rate = 1.0;
    }
    pace_rate += (target_rate - pace_rate) * 0.5;
    if (std::abs(pace_rate - 1.0) < 0.005) {
        pace_rate = 1.0;
    }
    pace_min_rate_since_log = (std::min)(pace_min_rate_since_log, pace_rate);

    if (pace_min_rate_since_log < 1.0 && now - pace_last_log_ns >= 2'000'000'000) {
        LOG_INFO(Audio_Sink,
                 "[Kledd] audio pace: game at {:.1f} fps of its usual {:.0f}, sound at {:.0f}% "
                 "(lowest {:.0f}% since the last report)",
                 fps, pace_target_fps, pace_rate * 100.0, pace_min_rate_since_log * 100.0);
        pace_min_rate_since_log = 1.0;
        pace_last_log_ns = now;
    }
    return pace_rate;
}

void SinkStream::NoteIssue(SinkIssue issue, u64 amount) {
    // Diagnostic only. Each kind of host-side trouble is counted, and one summary line is
    // logged at most every 2 seconds so a long stall does not flood the log.
    if (type != StreamType::Render) {
        return;
    }
    switch (issue) {
    case SinkIssue::Underrun:
        underrun_callbacks++;
        underrun_frames += amount;
        break;
    case SinkIssue::ShortPop:
        short_pops++;
        short_frames += amount;
        break;
    case SinkIssue::RingFull:
        ring_full_pushes++;
        dropped_frames += amount;
        break;
    case SinkIssue::CallbackGap: {
        callback_gaps++;
        u64 prev = callback_gap_max_ms.load();
        while (prev < amount && !callback_gap_max_ms.compare_exchange_weak(prev, amount)) {
        }
        break;
    }
    }

    const s64 now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
    s64 last = last_issue_log_ns.load();
    if (last != 0 && now_ns - last < 2000000000) {
        return;
    }
    if (!last_issue_log_ns.compare_exchange_strong(last, now_ns)) {
        return;
    }
    LOG_WARNING(Audio_Sink,
                "{}: host audio trouble: {} underrun callbacks ({} ms), {} short pops ({} ms), "
                "{} ring-full pushes ({} ms dropped), {} callback gaps (longest {} ms), "
                "{} buffers queued",
                name, underrun_callbacks.exchange(0),
                underrun_frames.exchange(0) * 1000 / TargetSampleRate, short_pops.exchange(0),
                short_frames.exchange(0) * 1000 / TargetSampleRate, ring_full_pushes.exchange(0),
                dropped_frames.exchange(0) * 1000 / TargetSampleRate, callback_gaps.exchange(0),
                callback_gap_max_ms.exchange(0), queued_buffers.load());
}

u64 SinkStream::GetExpectedPlayedSampleCount() {
    std::scoped_lock lk{sample_count_lock};
    auto cur_time{system.CoreTiming().GetGlobalTimeNs()};
    auto time_delta{cur_time - last_sample_count_update_time};
    auto exp_played_sample_count{min_played_sample_count + (TargetSampleRate * time_delta) / std::chrono::seconds{1}};

    // Add 25ms of latency in sample reporting to allow for some leeway in scheduler timings
    return std::min<u64>(exp_played_sample_count, max_played_sample_count) + TargetSampleCount * 5;
}

void SinkStream::WaitFreeSpace(std::stop_token stop_token) {
    std::unique_lock lk{release_mutex};

    auto can_continue = [this]() {
        return paused || queued_buffers < max_queue_size;
    };

    release_cv.wait_for(lk, std::chrono::milliseconds(7), can_continue);

    if (queued_buffers > max_queue_size + 3) {
        const auto wait_start = std::chrono::steady_clock::now();
        release_cv.wait(lk, stop_token, can_continue);
        // Diagnostic: the render thread sat here because the host callback was not draining.
        const auto waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - wait_start)
                                   .count();
        if (waited_ms >= 100) {
            LOG_WARNING(Audio_Sink,
                        "{}: audio render thread waited {} ms for the host sink to drain "
                        "({} buffers queued)",
                        name, waited_ms, queued_buffers.load());
        }
    }
}

void SinkStream::SignalPause() {
    {
        std::scoped_lock lk{release_mutex};
        paused = true;
    }
    release_cv.notify_one();
}

} // namespace AudioCore::Sink
