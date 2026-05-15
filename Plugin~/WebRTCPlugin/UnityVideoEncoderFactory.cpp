#include "pch.h"

#include <api/video_codecs/video_encoder.h>
#include <media/engine/internal_encoder_factory.h>
#include <modules/video_coding/include/video_error_codes.h>
#include <rtc_base/time_utils.h>
#include <tuple>

#include "Codec/CreateVideoCodecFactory.h"
#include "GraphicsDevice/GraphicsUtility.h"
#include "ProfilerMarkerFactory.h"
#include "ScopedProfiler.h"
#include "UnityVideoEncoderFactory.h"

namespace unity
{
namespace webrtc
{
    // [realmview fork] Window during which a second keyframe request is treated as
    // a duplicate of the first and downgraded to a delta. 100 ms covers the typical
    // DC + RTCP-PLI double-fire pattern documented in rec-e6ba141c81b5
    // (back-to-back IDRs at frames 9-10 and 236-237, each ~33 ms apart) without
    // suppressing legitimate emergency keyframe requests for *different* events
    // (which arrive seconds apart in practice). See webrtc_fork.md / "Encoder-level
    // keyframe dedup" for the rationale and tuning notes.
    static constexpr int64_t kKeyframeRequestDedupWindowMs = 100;

    class UnityVideoEncoder : public VideoEncoder
    {
    public:
        UnityVideoEncoder(std::unique_ptr<VideoEncoder> encoder, ProfilerMarkerFactory* profiler)
            : encoder_(std::move(encoder))
            , profiler_(profiler)
            , marker_(nullptr)
            , profilerThread_(nullptr)
            , last_keyframe_request_ms_(0)
            , dedup_suppressed_count_(0)
        {
            if (profiler)
                marker_ = profiler->CreateMarker(
                    "UnityVideoEncoder.Encode", kUnityProfilerCategoryOther, kUnityProfilerMarkerFlagDefault, 0);
        }
        ~UnityVideoEncoder() override { }

        void SetFecControllerOverride(FecControllerOverride* fec_controller_override) override
        {
            encoder_->SetFecControllerOverride(fec_controller_override);
        }
        int32_t InitEncode(const VideoCodec* codec_settings, int32_t number_of_cores, size_t max_payload_size) override
        {
            int32_t result = encoder_->InitEncode(codec_settings, number_of_cores, max_payload_size);
            if (result >= WEBRTC_VIDEO_CODEC_OK && !profilerThread_)
            {
                std::stringstream ss;
                ss << "Encoder ";
                ss
                    << (encoder_->GetEncoderInfo().implementation_name.empty()
                            ? "VideoEncoder"
                            : encoder_->GetEncoderInfo().implementation_name);
                ss << "(" << CodecTypeToPayloadString(codec_settings->codecType) << ")";
                profilerThread_ = profiler_->CreateScopedProfilerThread("WebRTC", ss.str().c_str());
            }

            return result;
        }
        int InitEncode(const VideoCodec* codec_settings, const VideoEncoder::Settings& settings) override
        {
            int result = encoder_->InitEncode(codec_settings, settings);
            if (result >= WEBRTC_VIDEO_CODEC_OK && !profilerThread_)
            {
                std::stringstream ss;
                ss << "Encoder ";
                ss
                    << (encoder_->GetEncoderInfo().implementation_name.empty()
                            ? "VideoEncoder"
                            : encoder_->GetEncoderInfo().implementation_name);
                ss << "(" << CodecTypeToPayloadString(codec_settings->codecType) << ")";
                profilerThread_ = profiler_->CreateScopedProfilerThread("WebRTC", ss.str().c_str());
            }

            return result;
        }
        int32_t RegisterEncodeCompleteCallback(EncodedImageCallback* callback) override
        {
            return encoder_->RegisterEncodeCompleteCallback(callback);
        }
        int32_t Release() override { return encoder_->Release(); }
        int32_t Encode(const VideoFrame& frame, const std::vector<VideoFrameType>* frame_types) override
        {
            // [realmview fork] Keyframe-request dedup chokepoint.
            //
            // libwebrtc has multiple INDEPENDENT paths that can mark the next frame
            // as a keyframe via `frame_types`:
            //   1. RTCP PLI from receiver  -> VideoStreamEncoder::SendKeyFrame()
            //                              -> next Encode() carries kVideoFrameKey
            //   2. Our SenderGenerateKeyFrame export (Intervention A, the cheap
            //      C# path) -> same flow via RtpSenderInterface::GenerateKeyFrame()
            //   3. Internal libwebrtc keyframe requesters (rare on our codepath)
            //
            // When the RealmVault server fires both DC `request_keyframe` AND RTCP
            // PLI for the SAME loss event (which it does -- see rec-e6ba141c81b5,
            // pli_sent=7 / keyframe_requests=7 in 16 s), both signals reach the
            // encoder within ~33 ms of each other and produce back-to-back IDRs
            // (gap=1 in the recording logs). The C# predictive suppression in
            // WebRTCStreamer.HandleControlChannelMessage covers the DC side but
            // CANNOT see the RTCP PLI path -- that one is handled entirely inside
            // libwebrtc native code.
            //
            // This wrapper is the single chokepoint where every explicit keyframe
            // request reaches the actual encoder (the Android H.264
            // HardwareVideoEncoder for our Quest 3 case). If two requests arrive
            // within kKeyframeRequestDedupWindowMs, we honor the first and
            // downgrade the second to kVideoFrameDelta. The encoder then produces
            // a P-frame for the duplicate instead of stacking another ~80 KB IDR.
            //
            // Natural GOP boundaries are NOT affected: MediaCodec's internal
            // KEY_FRAME_INTERVAL timer emits IDRs without going through
            // `frame_types`, so this code only suppresses *explicit* requests.
            const std::vector<VideoFrameType>* effective_frame_types = frame_types;
            std::vector<VideoFrameType> deduped_storage;
            if (frame_types != nullptr && !frame_types->empty())
            {
                bool wants_key = false;
                for (const auto& t : *frame_types)
                {
                    if (t == VideoFrameType::kVideoFrameKey)
                    {
                        wants_key = true;
                        break;
                    }
                }
                if (wants_key)
                {
                    const int64_t now_ms = rtc::TimeMillis();
                    const int64_t since_last_ms = now_ms - last_keyframe_request_ms_;
                    if (last_keyframe_request_ms_ != 0 && since_last_ms < kKeyframeRequestDedupWindowMs)
                    {
                        deduped_storage = *frame_types;
                        for (auto& t : deduped_storage)
                        {
                            if (t == VideoFrameType::kVideoFrameKey)
                                t = VideoFrameType::kVideoFrameDelta;
                        }
                        effective_frame_types = &deduped_storage;
                        ++dedup_suppressed_count_;
                        // Rate-limited: log every 8th suppression so we get a
                        // signal that dedup is working without flooding logcat
                        // during sustained loss events.
                        if ((dedup_suppressed_count_ & 0x7) == 1)
                        {
                            RTC_LOG(LS_INFO)
                                << "[realmview] UnityVideoEncoder: deduped keyframe request"
                                << " (since_last_ms=" << since_last_ms
                                << " window=" << kKeyframeRequestDedupWindowMs
                                << "ms total_suppressed=" << dedup_suppressed_count_
                                << "). Downgraded kVideoFrameKey -> kVideoFrameDelta to avoid"
                                << " back-to-back IDR.";
                        }
                    }
                    else
                    {
                        last_keyframe_request_ms_ = now_ms;
                    }
                }
            }

            int32_t result;
            {
                std::unique_ptr<const ScopedProfiler> profiler;
                if (profiler_)
                    profiler = profiler_->CreateScopedProfiler(*marker_);
                result = encoder_->Encode(frame, effective_frame_types);
            }
            return result;
        }
        void SetRates(const RateControlParameters& parameters) override { encoder_->SetRates(parameters); }
        void OnPacketLossRateUpdate(float packet_loss_rate) override
        {
            encoder_->OnPacketLossRateUpdate(packet_loss_rate);
        }
        void OnRttUpdate(int64_t rtt_ms) override { encoder_->OnRttUpdate(rtt_ms); }
        void OnLossNotification(const LossNotification& loss_notification) override
        {
            encoder_->OnLossNotification(loss_notification);
        }
        EncoderInfo GetEncoderInfo() const override { return encoder_->GetEncoderInfo(); }

    private:
        std::unique_ptr<VideoEncoder> encoder_;
        ProfilerMarkerFactory* profiler_;
        const UnityProfilerMarkerDesc* marker_;
        std::unique_ptr<const ScopedProfilerThread> profilerThread_;

        // [realmview fork] Keyframe-request dedup state. Only accessed from the
        // encoder thread (libwebrtc serialises VideoEncoder::Encode calls per
        // stream), so plain int64_t is sufficient -- no atomics needed.
        // last_keyframe_request_ms_ = 0 marks "no prior request observed",
        // which lets the very first IDR (initial bitstream + SPS/PPS) through
        // unconditionally.
        int64_t last_keyframe_request_ms_;
        int64_t dedup_suppressed_count_;
    };

    UnityVideoEncoderFactory::UnityVideoEncoderFactory(IGraphicsDevice* gfxDevice, ProfilerMarkerFactory* profiler)
        : profiler_(profiler)
        , factories_()
    {
        const std::vector<std::string> arrayImpl = {
            kInternalImpl, kNvCodecImpl, kAndroidMediaCodecImpl, kVideoToolboxImpl
        };

        for (auto impl : arrayImpl)
        {
            auto factory = CreateVideoEncoderFactory(impl, gfxDevice, profiler);
            if (factory)
                factories_.emplace(impl, factory);
        }
    }

    UnityVideoEncoderFactory::~UnityVideoEncoderFactory() = default;

    std::vector<webrtc::SdpVideoFormat> UnityVideoEncoderFactory::GetSupportedFormats() const
    {
        std::vector<SdpVideoFormat> supported_codecs = GetSupportedFormatsInFactories(factories_);

        // Set video codec order: default video codec is VP8
        auto findIndex = [&](webrtc::SdpVideoFormat& format) -> long {
            const std::string sortOrder[4] = { "VP8", "VP9", "H264", "AV1X" };
            auto it = std::find(std::begin(sortOrder), std::end(sortOrder), format.name);
            if (it == std::end(sortOrder))
                return LONG_MAX;
            return static_cast<long>(std::distance(std::begin(sortOrder), it));
        };
        std::sort(
            supported_codecs.begin(),
            supported_codecs.end(),
            [&](webrtc::SdpVideoFormat& x, webrtc::SdpVideoFormat& y) -> int { return (findIndex(x) < findIndex(y)); });
        return supported_codecs;
    }

    webrtc::VideoEncoderFactory::CodecSupport UnityVideoEncoderFactory::QueryCodecSupport(
        const SdpVideoFormat& format, absl::optional<std::string> scalability_mode) const
    {
        VideoEncoderFactory* factory = FindCodecFactory(factories_, format);
        RTC_DCHECK(format.IsCodecInList(factory->GetSupportedFormats()));
        return factory->QueryCodecSupport(format, scalability_mode);
    }

    std::unique_ptr<webrtc::VideoEncoder>
    UnityVideoEncoderFactory::CreateVideoEncoder(const webrtc::SdpVideoFormat& format)
    {
        VideoEncoderFactory* factory = FindCodecFactory(factories_, format);
        auto encoder = factory->CreateVideoEncoder(format);

        // [realmview fork] ALWAYS wrap, regardless of whether the Unity Profiler is
        // attached. The wrapper now performs keyframe-request deduplication (see the
        // dedup block in UnityVideoEncoder::Encode), which is the single chokepoint
        // we have for catching DC + RTCP-PLI double-fires that the C# layer cannot
        // intercept. The profiler hooks inside the wrapper are themselves gated on
        // profiler_ != nullptr, so this wrap-unconditionally change has near-zero
        // cost when no profiler is attached (one extra virtual call + one branch
        // check per frame). Pre-realmview behaviour returned the bare encoder when
        // profiler_ was null, which meant release builds completely skipped the
        // dedup -- the opposite of what we want.
        return std::make_unique<UnityVideoEncoder>(std::move(encoder), profiler_);
    }
}
}
