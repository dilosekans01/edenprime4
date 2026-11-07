// SPDX-FileCopyrightText: Copyright 2026 eden-kledd
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace AudioCore::Sink {

/**
 * eden-kledd: WSOLA time stretch. Plays interleaved 16-bit audio slower than it arrives without
 * lowering its pitch: 20 ms Hann windows are overlap-added every 10 ms of output, each taken
 * from where the input lines up best (within +-5 ms) with the continuation of the previous one.
 *
 * At rate 1 each window starts exactly where the previous one continues, and Hann windows at
 * half overlap sum to one, so the output is the input unchanged, 20 ms later. The stretcher can
 * therefore stay in the path all the time and move in and out of slow motion without a seam.
 */
class KleddTimeStretch {
public:
    static constexpr size_t Hop = 480;     ///< output frames per window step (10 ms at 48 kHz)
    static constexpr size_t Window = Hop * 2;
    static constexpr size_t Search = 240;  ///< analysis position tolerance in frames (5 ms)

    /**
     * Produces num_frames output frames into out. pop(dst, frames) must write exactly that many
     * interleaved frames into dst (filling underruns itself) and return how many really came
     * from the source. Returns the frames really taken from the source during this call.
     * rate is the playback speed, 0 < rate <= 1.
     */
    template <typename Pop>
    size_t Process(int16_t* out, size_t num_frames, size_t channels, double rate, Pop&& pop) {
        size_t popped = 0;
        if (channels != ch) {
            Reset(channels);
        }
        rate = std::clamp(rate, 0.25, 1.0);
        while (OutFrames() < num_frames) {
            Step(rate, pop, popped);
        }
        const float* src = &done[done_read * ch];
        for (size_t i = 0; i < num_frames * ch; ++i) {
            const float v = std::round(src[i]);
            out[i] = static_cast<int16_t>(std::clamp(v, -32768.0f, 32767.0f));
        }
        done_read += num_frames;
        if (done_read * ch * 2 > done.size() || done_read > Window * 4) {
            done.erase(done.begin(), done.begin() + static_cast<std::ptrdiff_t>(done_read * ch));
            done_read = 0;
        }
        return popped;
    }

private:
    void Reset(size_t channels) {
        ch = channels;
        input.clear();
        in_base = 0;
        in_end = 0;
        accum.assign(Window * ch, 0.0f);
        done.clear();
        done_read = 0;
        nominal = 0.0;
        actual = 0;
        first = true;
        window.resize(Window);
        constexpr double pi = 3.14159265358979323846;
        for (size_t i = 0; i < Window; ++i) {
            // Periodic Hann: copies one Hop apart sum to exactly one
            window[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * pi * static_cast<double>(i) /
                                                                static_cast<double>(Window)));
        }
    }

    size_t OutFrames() const {
        return done.size() / ch - done_read;
    }

    template <typename Pop>
    void EnsureInput(uint64_t until, Pop& pop, size_t& popped) {
        if (in_end >= until) {
            return;
        }
        const size_t need = static_cast<size_t>(until - in_end);
        scratch.resize(need * ch);
        popped += pop(scratch.data(), need);
        const size_t old = input.size();
        input.resize(old + need * ch);
        for (size_t i = 0; i < need * ch; ++i) {
            input[old + i] = static_cast<float>(scratch[i]);
        }
        in_end += need;
    }

    float Sample(uint64_t frame, size_t c) const {
        return input[static_cast<size_t>(frame - in_base) * ch + c];
    }

    template <typename Pop>
    void Step(double rate, Pop& pop, size_t& popped) {
        uint64_t pos;
        if (first) {
            pos = 0;
            first = false;
        } else if (rate >= 1.0) {
            // Seamless continuation: the output is the input, delayed
            pos = actual + Hop;
            nominal = static_cast<double>(pos);
        } else {
            nominal += static_cast<double>(Hop) * rate;
            const uint64_t natural = actual + Hop;
            const uint64_t centre = static_cast<uint64_t>(nominal);
            const uint64_t lo = (std::max)(centre > Search ? centre - Search : 0, in_base);
            const uint64_t hi = centre + Search;
            EnsureInput((std::max)(hi, natural) + Window, pop, popped);
            // Normalised cross-correlation of each candidate's first half with the second half
            // of the previous window (the natural continuation), on a mono mix, every other
            // sample.
            double best_score = -2.0;
            pos = (std::min)(natural, (std::max)(centre, lo));
            for (uint64_t c = lo; c <= hi; ++c) {
                double xy = 0.0;
                double xx = 0.0;
                double yy = 0.0;
                for (size_t i = 0; i < Hop; i += 2) {
                    float x = 0.0f;
                    float y = 0.0f;
                    for (size_t k = 0; k < ch; ++k) {
                        x += Sample(c + i, k);
                        y += Sample(natural + i, k);
                    }
                    xy += static_cast<double>(x) * y;
                    xx += static_cast<double>(x) * x;
                    yy += static_cast<double>(y) * y;
                }
                const double denom = std::sqrt(xx * yy);
                const double score = denom > 1e-3 ? xy / denom : 0.0;
                // Prefer the nominal position on ties (silence) so time keeps its pace
                const double bias =
                    -1e-6 * std::abs(static_cast<double>(c) - static_cast<double>(centre));
                if (score + bias > best_score) {
                    best_score = score + bias;
                    pos = c;
                }
            }
        }
        EnsureInput(pos + Window, pop, popped);
        for (size_t i = 0; i < Window; ++i) {
            const float w = window[i];
            for (size_t k = 0; k < ch; ++k) {
                accum[i * ch + k] += w * Sample(pos + i, k);
            }
        }
        done.insert(done.end(), accum.begin(), accum.begin() + static_cast<std::ptrdiff_t>(Hop * ch));
        std::memmove(accum.data(), accum.data() + Hop * ch, Hop * ch * sizeof(float));
        std::fill(accum.begin() + static_cast<std::ptrdiff_t>(Hop * ch), accum.end(), 0.0f);
        actual = pos;

        // Input before both the next natural continuation and the next search range is unused
        const uint64_t centre_next = static_cast<uint64_t>(nominal);
        const uint64_t keep_from =
            (std::min)(actual, centre_next > Search ? centre_next - Search : 0);
        if (keep_from > in_base + Window * 4) {
            const size_t drop = static_cast<size_t>(keep_from - in_base);
            input.erase(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(drop * ch));
            in_base = keep_from;
        }
    }

    size_t ch = 0;
    std::vector<float> input;   ///< interleaved; input[0] is frame in_base of the source
    uint64_t in_base = 0;
    uint64_t in_end = 0;
    std::vector<float> accum;   ///< overlap-add of the last window
    std::vector<float> done;    ///< finished output frames
    size_t done_read = 0;
    double nominal = 0.0;       ///< where the source would be read at the requested rate
    uint64_t actual = 0;        ///< start of the last window taken
    bool first = true;
    std::vector<float> window;
    std::vector<int16_t> scratch;
};

} // namespace AudioCore::Sink
