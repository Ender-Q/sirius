// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/settings.h"
#include "video_core/host1x/codecs/decoder.h"
#include "video_core/host1x/host1x.h"
#include "video_core/memory_manager.h"

namespace Tegra {

    Decoder::Decoder(Host1x::Host1x& host1x_, s32 id_, const Host1x::NvdecCommon::NvdecRegisters& regs_,
                     Host1x::FrameQueue& frame_queue_)
    : host1x(host1x_), memory_manager{host1x.GMMU()}, regs{regs_}, id{id_}, frame_queue{
        frame_queue_} {}

        Decoder::~Decoder() = default;

        void Decoder::Decode() {
            if (!initialized) {
                return;
            }

            const auto packet_data = ComposeFrame();

           // Capture the state needed for queuing BEFORE sending the packet
           // and potentially yielding. The main `regs` and `current_context` can be
           // overwritten by the time FFmpeg returns a frame.
           const bool is_interlaced_frame = IsInterlaced();
           const auto interlaced_offsets = GetInterlacedOffsets();
           const auto progressive_offsets = GetProgressiveOffsets();

            // Send assembled bitstream to decoder.
            if (!decode_api.SendPacket(packet_data)) {
                return;
            }

            // Only process visible frames.
            if (vp9_hidden_frame) {
                return;
            }

            // Receive output frames from decoder.
            // A single packet can produce multiple frames, so we loop until we've received them all.
            while (true) {
                auto frame = decode_api.ReceiveFrame();
                if (!frame) { // No more frames available for now.
                    break;
                }

                if (is_interlaced_frame) {
                    auto [luma_top, luma_bottom, chroma_top, chroma_bottom] = interlaced_offsets;
                    auto frame_copy = frame;
                    frame_queue.PushDecodeOrder(id, luma_top, std::move(frame));
                    frame_queue.PushDecodeOrder(id, luma_bottom, std::move(frame_copy));
                } else {
                    auto [luma_offset, chroma_offset] = progressive_offsets;
                    frame_queue.PushDecodeOrder(id, luma_offset, std::move(frame));
                }
            }
        }
} // namespace Tegra
