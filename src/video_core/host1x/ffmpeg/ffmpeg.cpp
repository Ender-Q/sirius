// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/memory.h"
#include "video_core/host1x/ffmpeg/ffmpeg.h"
#include "video_core/memory_manager.h"

extern "C" {
    #ifdef LIBVA_FOUND
    // for querying VAAPI driver information
    #include <libavutil/hwcontext_vaapi.h>
    #endif
}

namespace FFmpeg {

    namespace {

        void FfmpegLog(void* ptr, int level, const char* fmt, va_list vl) {
            if (level > av_log_get_level()) {
                return;
            }

            char line[1024];
            vsnprintf(line, sizeof(line), fmt, vl);

            // Remove trailing newline
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] == '\n') {
                line[len - 1] = '\0';
            }

            // Map FFmpeg log levels to yuzu log levels.
            switch (level) {
            case AV_LOG_PANIC:
            case AV_LOG_FATAL:
            case AV_LOG_ERROR:
                LOG_ERROR(HW_GPU, "FFmpeg: {}", line);
                break;
            case AV_LOG_WARNING:
                LOG_WARNING(HW_GPU, "FFmpeg: {}", line);
                break;
            default:
                LOG_INFO(HW_GPU, "FFmpeg: {}", line);
                break;
            }
        }

        constexpr AVPixelFormat PreferredGpuFormat = AV_PIX_FMT_NV12;
        constexpr AVPixelFormat PreferredCpuFormat = AV_PIX_FMT_YUV420P;
        constexpr std::array PreferredGpuDecoders = {
            AV_HWDEVICE_TYPE_CUDA,
            #ifdef _WIN32
            AV_HWDEVICE_TYPE_D3D11VA,
            AV_HWDEVICE_TYPE_DXVA2,
            #elif defined(__unix__)
            AV_HWDEVICE_TYPE_VAAPI,
            AV_HWDEVICE_TYPE_VDPAU,
            #endif
            AV_HWDEVICE_TYPE_VULKAN
        };

        AVPixelFormat GetGpuFormat(AVCodecContext* codec_context, const AVPixelFormat* pix_fmts) {
            for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
                // The initial format from hw_config is an opaque type like AV_PIX_FMT_VAAPI.
                // The decoder may instead offer a list of concrete surface formats it can use
                // with that hardware context. We need to find a compatible one.
                // For VA-API, NV12 is the common hardware surface format.
                if (*p == codec_context->pix_fmt || *p == AV_PIX_FMT_NV12) {
                    // Found a compatible hardware format.
                    LOG_INFO(HW_GPU, "FFmpeg: Selected hardware pixel format {}.",
                             av_get_pix_fmt_name(*p));
                    return *p;
                }
            }

            // The decoder does not support the requested hardware format for this stream.
            // Build a list of supported formats for the log message.
            std::string supported_formats_str;
            for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
                supported_formats_str += av_get_pix_fmt_name(*p);
                if (p[1] != AV_PIX_FMT_NONE) {
                    supported_formats_str += ", ";
                }
            }

            const AVHWDeviceContext* device_ctx =
                reinterpret_cast<const AVHWDeviceContext*>(codec_context->hw_device_ctx->data);

            LOG_WARNING(HW_GPU,
                        "Hardware decoder '{}' on device '{}' does not support format '{}' for this "
                        "stream. Supported formats: [{}]. Falling back to software decoding.",
                        codec_context->codec->name, av_hwdevice_get_type_name(device_ctx->type),
                        av_get_pix_fmt_name(codec_context->pix_fmt), supported_formats_str);

            // Fallback to software.
            av_buffer_unref(&codec_context->hw_device_ctx);

            // Check if the preferred software format is supported.
            for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
                if (*p == PreferredCpuFormat) {
                    codec_context->pix_fmt = PreferredCpuFormat;
                    return PreferredCpuFormat;
                }
            }

            LOG_ERROR(HW_GPU, "Decoder does not support preferred software format {}. Decoding will likely fail.",
                      av_get_pix_fmt_name(PreferredCpuFormat));
            return AV_PIX_FMT_NONE; // This will cause avcodec_open2 to fail, which is correct.
        }

        std::string AVError(int errnum) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE] = {};
            av_make_error_string(errbuf, sizeof(errbuf) - 1, errnum);
            return errbuf;
        }

    } // namespace

    Packet::Packet(std::span<const u8> data) {
        m_packet = av_packet_alloc();
        m_packet->data = const_cast<u8*>(data.data());
        m_packet->size = static_cast<s32>(data.size());
    }

    Packet::~Packet() {
        av_packet_free(&m_packet);
    }

    Frame::Frame() {
        m_frame = av_frame_alloc();
    }

    Frame::~Frame() {
        av_frame_free(&m_frame);
    }

    Decoder::Decoder(Tegra::Host1x::NvdecCommon::VideoCodec codec) {
        const AVCodecID av_codec = [&] {
            switch (codec) {
                case Tegra::Host1x::NvdecCommon::VideoCodec::H264:
                    return AV_CODEC_ID_H264;
                case Tegra::Host1x::NvdecCommon::VideoCodec::VP8:
                    return AV_CODEC_ID_VP8;
                case Tegra::Host1x::NvdecCommon::VideoCodec::VP9:
                    return AV_CODEC_ID_VP9;
                default:
                    UNIMPLEMENTED_MSG("Unknown codec {}", codec);
                    return AV_CODEC_ID_NONE;
            }
        }();

        m_codec = avcodec_find_decoder(av_codec);
        ASSERT_MSG(m_codec, "Failed to find decoder for AVCodecID {}", av_codec);
    }

    bool Decoder::SupportsDecodingOnDevice(AVPixelFormat* out_pix_fmt, AVHWDeviceType type) const {
        for (int i = 0;; i++) {
            const AVCodecHWConfig* config = avcodec_get_hw_config(m_codec, i);
            if (!config) {
                LOG_DEBUG(HW_GPU, "{} decoder does not support device type {}", m_codec->name, av_hwdevice_get_type_name(type));
                break;
            }
            if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX && config->device_type == type) {
                LOG_INFO(HW_GPU, "Using {} GPU decoder", av_hwdevice_get_type_name(type));
                *out_pix_fmt = config->pix_fmt;
                return true;
            }
        }

        return false;
    }

    std::vector<AVHWDeviceType> HardwareContext::GetSupportedDeviceTypes() {
        std::vector<AVHWDeviceType> types;
        AVHWDeviceType current_device_type = AV_HWDEVICE_TYPE_NONE;

        while (true) {
            current_device_type = av_hwdevice_iterate_types(current_device_type);
            if (current_device_type == AV_HWDEVICE_TYPE_NONE) {
                return types;
            }

            types.push_back(current_device_type);
        }
    }

    HardwareContext::~HardwareContext() {
        av_buffer_unref(&m_gpu_decoder);
    }

    bool HardwareContext::InitializeForDecoder(DecoderContext& decoder_context, const Decoder& decoder) {
        const auto supported_types = GetSupportedDeviceTypes();
        for (const auto type : PreferredGpuDecoders) {
            AVPixelFormat hw_pix_fmt;

            if (std::ranges::find(supported_types, type) == supported_types.end()) {
                LOG_DEBUG(HW_GPU, "{} explicitly unsupported", av_hwdevice_get_type_name(type));
                continue;
            }

            if (!this->InitializeWithType(type)) {
                continue;
            }

            if (decoder.SupportsDecodingOnDevice(&hw_pix_fmt, type)) {
                decoder_context.InitializeHardwareDecoder(*this, hw_pix_fmt);
                return true;
            }
        }

        return false;
    }

    bool HardwareContext::InitializeWithType(AVHWDeviceType type) {
        av_buffer_unref(&m_gpu_decoder);

        if (const int ret = av_hwdevice_ctx_create(&m_gpu_decoder, type, nullptr, nullptr, 0); ret < 0) {
            LOG_DEBUG(HW_GPU, "av_hwdevice_ctx_create({}) failed: {}", av_hwdevice_get_type_name(type), AVError(ret));
            return false;
        }

        #ifdef LIBVA_FOUND
        if (type == AV_HWDEVICE_TYPE_VAAPI) {
            // We need to determine if this is an impersonated VAAPI driver.
            auto* hwctx = reinterpret_cast<AVHWDeviceContext*>(m_gpu_decoder->data);
            auto* vactx = static_cast<AVVAAPIDeviceContext*>(hwctx->hwctx);
            const char* vendor_name = vaQueryVendorString(vactx->display);
            if (strstr(vendor_name, "VDPAU backend")) {
                // VDPAU impersonated VAAPI impls are super buggy, we need to skip them.
                LOG_DEBUG(HW_GPU, "Skipping VDPAU impersonated VAAPI driver");
                return false;
            } else {
                // According to some user testing, certain VAAPI drivers (Intel?) could be buggy.
                // Log the driver name just in case.
                LOG_DEBUG(HW_GPU, "Using VAAPI driver: {}", vendor_name);
            }
        }
        #endif

        return true;
    }

    DecoderContext::DecoderContext(const Decoder& decoder) : m_decoder{decoder} {
        m_codec_context = avcodec_alloc_context3(m_decoder.GetCodec());
        ASSERT(m_codec_context); // Ensure allocation was successful

        // Use av_opt_set_int and av_opt_set to set options
        // "preset" and "tune" are codec-private options, so they still apply to m_codec_context->priv_data.
        av_opt_set(m_codec_context->priv_data, "preset", "veryfast", 0);
        av_opt_set(m_codec_context->priv_data, "tune", "zerolatency", 0);

        // Setting thread_count and thread_type using AVCodecContext members directly
        // The previous usage of FF_THREAD_FRAME was from codec_internal.h.
        // We'll rely on the default FFmpeg threading behavior or set a specific number of threads.
        // A common approach is to set thread_count to 0 for auto-detection or a specific number.
        // Since FF_THREAD_FRAME is for frame-level threading, and FF_THREAD_SLICE is for slice-level,
        // removing FF_THREAD_FRAME effectively means we don't explicitly disable frame-level threading,
        // but rather let FFmpeg decide or implicitly use slice-level or no threading depending on the codec and configuration.
        // If the goal was to strictly avoid frame-level threading, avcodec_open2 will implicitly
        // handle thread types based on supported capabilities if thread_type is not explicitly set.
        // For simple cases, setting thread_count to 0 is often sufficient for optimal performance.
        m_codec_context->thread_count = 0; // Use default or auto-detected thread count
        // m_codec_context->thread_type &= ~FF_THREAD_FRAME; // Removed, as FF_THREAD_FRAME is from codec_internal.h
    }

    DecoderContext::~DecoderContext() {
        av_buffer_unref(&m_codec_context->hw_device_ctx);
        avcodec_free_context(&m_codec_context);
    }

    void DecoderContext::InitializeHardwareDecoder(const HardwareContext& context, AVPixelFormat hw_pix_fmt) {
        m_codec_context->hw_device_ctx = av_buffer_ref(context.GetBufferRef());
        m_codec_context->get_format = GetGpuFormat;
        m_codec_context->pix_fmt = hw_pix_fmt;
    }

    bool DecoderContext::OpenContext(const Decoder& decoder) {
        if (const int ret = avcodec_open2(m_codec_context, decoder.GetCodec(), nullptr); ret < 0) {
            LOG_ERROR(HW_GPU, "avcodec_open2 error: {}", AVError(ret));
            return false;
        }

        if (!m_codec_context->hw_device_ctx) {
            LOG_INFO(HW_GPU, "Using FFmpeg software decoding");
        }

        return true;
    }

    bool DecoderContext::SendPacket(const Packet& packet) {
        if (const int ret = avcodec_send_packet(m_codec_context, packet.GetPacket()); ret < 0) {
            LOG_ERROR(HW_GPU, "avcodec_send_packet error: {}", AVError(ret));
            return false;
        }

        return true;
    }

    std::shared_ptr<Frame> DecoderContext::ReceiveFrame() {
        auto received_frame = std::make_shared<Frame>();

        const int ret = avcodec_receive_frame(m_codec_context, received_frame->GetFrame());
        if (ret < 0) {
            if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
                LOG_ERROR(HW_GPU, "avcodec_receive_frame error: {}", AVError(ret));
            }
            return {};
        }

        std::shared_ptr<Frame> output_frame;

        if (received_frame->IsHardwareDecoded()) {
            // Hardware frame was successfully decoded, transfer it to system memory.
            output_frame = std::make_shared<Frame>();

            // Transfer to NV12, as the VIC pipeline can handle it.
            output_frame->GetFrame()->format = PreferredGpuFormat;

            if (const int transfer_ret =
                    av_hwframe_transfer_data(output_frame->GetFrame(), received_frame->GetFrame(), 0);
                transfer_ret < 0) {
                LOG_ERROR(HW_GPU, "Failed to transfer hardware frame to system memory: {}",
                          AVError(transfer_ret));
                return {};
            }
        } else {
            // Frame is already in system memory (software frame). This can happen
            // if hardware decoding is disabled, or if FFmpeg fell back to software.
            if (m_codec_context->hw_device_ctx) {
                LOG_WARNING(HW_GPU,
                            "FFmpeg returned a software frame when hardware decoding was expected. "
                            "Format: {}. This may be due to unsupported video parameters.",
                            av_get_pix_fmt_name(received_frame->GetPixelFormat()));
            }
            output_frame = received_frame;
        }

        // The original code toggled the interlaced flag. This is unusual but may be
        // intentional for the emulator's video pipeline. This behavior is preserved.
    #if defined(FF_API_INTERLACED_FRAME) || LIBAVUTIL_VERSION_MAJOR >= 59
        if (output_frame->GetFrame()->flags & AV_FRAME_FLAG_INTERLACED) {
            output_frame->GetFrame()->flags &= ~AV_FRAME_FLAG_INTERLACED;
        } else {
            output_frame->GetFrame()->flags |= AV_FRAME_FLAG_INTERLACED;
        }
    #else
        output_frame->GetFrame()->interlaced_frame = !output_frame->GetFrame()->interlaced_frame;
    #endif

        return output_frame;
    }

    void DecodeApi::Reset() {
        m_hardware_context.reset();
        m_decoder_context.reset();
        m_decoder.reset();
    }

    bool DecodeApi::Initialize(Tegra::Host1x::NvdecCommon::VideoCodec codec) {
        av_log_set_callback(FfmpegLog);
        av_log_set_level(AV_LOG_DEBUG);

        this->Reset();
        m_decoder.emplace(codec);
        m_decoder_context.emplace(*m_decoder);

        // Enable GPU decoding if requested.
        if (Settings::values.nvdec_emulation.GetValue() == Settings::NvdecEmulation::Gpu) {
            m_hardware_context.emplace();
            m_hardware_context->InitializeForDecoder(*m_decoder_context, *m_decoder);
        }

        // Open the decoder context.
        if (!m_decoder_context->OpenContext(*m_decoder)) {
            this->Reset();
            return false;
        }

        return true;
    }

    bool DecodeApi::SendPacket(std::span<const u8> packet_data) {
        FFmpeg::Packet packet(packet_data);
        return m_decoder_context->SendPacket(packet);
    }

    std::shared_ptr<Frame> DecodeApi::ReceiveFrame() {
        // Receive raw frame from decoder.
        return m_decoder_context->ReceiveFrame();
    }

} // namespace FFmpeg
