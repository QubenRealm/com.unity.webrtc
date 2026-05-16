#include "pch.h"

#include <api/video/video_frame.h>

#include "VideoFrameAdapter.h"

namespace unity
{
namespace webrtc
{
    template<typename T>
    bool Contains(rtc::ArrayView<T> arr, T value)
    {
        for (auto e : arr)
        {
            if (e == value)
                return true;
        }
        return false;
    }

    ::webrtc::VideoFrame VideoFrameAdapter::CreateVideoFrame(rtc::scoped_refptr<VideoFrame> frame)
    {
        rtc::scoped_refptr<VideoFrameAdapter> adapter(new rtc::RefCountedObject<VideoFrameAdapter>(std::move(frame)));

        return ::webrtc::VideoFrame::Builder().set_video_frame_buffer(adapter).build();
    }

    VideoFrameAdapter::ScaledBuffer::ScaledBuffer(rtc::scoped_refptr<VideoFrameAdapter> parent, int width, int height)
        : parent_(parent)
        , width_(width)
        , height_(height)
    {
    }

    VideoFrameAdapter::ScaledBuffer::~ScaledBuffer() { }

    VideoFrameBuffer::Type VideoFrameAdapter::ScaledBuffer::type() const { return parent_->type(); }

    rtc::scoped_refptr<webrtc::I420BufferInterface> VideoFrameAdapter::ScaledBuffer::ToI420()
    {
        // [realmview fork] PM #12 H12 minimal: null-defend against post-teardown frames.
        // See VideoFrameAdapter::ToI420 for the full crash rationale.
        auto buffer = parent_->GetOrCreateFrameBufferForSize(Size(width_, height_));
        return buffer ? buffer->ToI420() : nullptr;
    }

    const I420BufferInterface* VideoFrameAdapter::ScaledBuffer::GetI420() const
    {
        auto buffer = parent_->GetOrCreateFrameBufferForSize(Size(width_, height_));
        return buffer ? buffer->GetI420() : nullptr;
    }

    rtc::scoped_refptr<VideoFrameBuffer>
    VideoFrameAdapter::ScaledBuffer::GetMappedFrameBuffer(rtc::ArrayView<VideoFrameBuffer::Type> types)
    {
        auto buffer = parent_->GetOrCreateFrameBufferForSize(Size(width_, height_));
        return Contains(types, buffer->type()) ? buffer : nullptr;
    }

    rtc::scoped_refptr<VideoFrameBuffer> VideoFrameAdapter::ScaledBuffer::CropAndScale(
        int offset_x, int offset_y, int crop_width, int crop_height, int scaled_width, int scaled_height)
    {
        return rtc::make_ref_counted<ScaledBuffer>(
            rtc::scoped_refptr<VideoFrameAdapter>(parent_), scaled_width, scaled_height);
    }

    VideoFrameAdapter::VideoFrameAdapter(rtc::scoped_refptr<VideoFrame> frame)
        : frame_(std::move(frame))
        , size_(frame_->size())
    {
    }

    VideoFrameBuffer::Type VideoFrameAdapter::type() const
    {
#if UNITY_IOS || UNITY_OSX || UNITY_ANDROID
        // todo(kazuki): support for kNative type for mobile platform and macOS.
        // Need to pass ObjCFrameBuffer instead of VideoFrameAdapter on macOS/iOS.
        // Need to pass AndroidVideoBuffer instead of VideoFrameAdapter on Android.
        return VideoFrameBuffer::Type::kI420;
#else
        return VideoFrameBuffer::Type::kNative;
#endif
    }

    const I420BufferInterface* VideoFrameAdapter::GetI420() const
    {
        // [realmview fork] PM #12 H12 minimal: null-defend.
        auto buffer = ConvertToVideoFrameBuffer(frame_);
        return buffer ? buffer->GetI420() : nullptr;
    }

    rtc::scoped_refptr<I420BufferInterface> VideoFrameAdapter::ToI420()
    {
        // [realmview fork] PM #12 H12 minimal: null-defend against post-teardown frames.
        // Returning nullptr lets Kazuki's fix_android_videoencoder.patch (in libwebrtc.so)
        // turn this into WEBRTC_VIDEO_CODEC_ERROR (drop one frame) instead of the original
        // SIGSEGV at libwebrtc.so+0x4ba258 in EncoderQueue (tombstone _22, build e50bb065,
        // 2026-05-16 22:34:04). Stock upstream had RTC_DCHECK only -- compiled out in
        // release builds, leaving the gmb->ToI420() result unchecked one line later.
        auto buffer = ConvertToVideoFrameBuffer(frame_);
        return buffer ? buffer->ToI420() : nullptr;
    }

    rtc::scoped_refptr<VideoFrameBuffer> VideoFrameAdapter::CropAndScale(
        int offset_x, int offset_y, int crop_width, int crop_height, int scaled_width, int scaled_height)
    {
        return rtc::make_ref_counted<ScaledBuffer>(
            rtc::scoped_refptr<VideoFrameAdapter>(this), scaled_width, scaled_height);
    }

    rtc::scoped_refptr<VideoFrameBuffer> VideoFrameAdapter::GetOrCreateFrameBufferForSize(const Size& size)
    {
        std::unique_lock<std::mutex> guard(scaleLock_);

        for (auto scaledI420buffer : scaledI40Buffers_)
        {
            Size bufferSize(scaledI420buffer->width(), scaledI420buffer->height());
            if (size == bufferSize)
            {
                return scaledI420buffer;
            }
        }
        auto buffer = VideoFrameBuffer::CropAndScale(0, 0, width(), height(), size.width(), size.height());
        // [realmview fork] PM #12 H12 minimal: don't cache nulls; they happen on teardown.
        if (!buffer) return nullptr;
        scaledI40Buffers_.push_back(buffer);
        return buffer;
    }

    rtc::scoped_refptr<I420BufferInterface>
    VideoFrameAdapter::ConvertToVideoFrameBuffer(rtc::scoped_refptr<VideoFrame> video_frame) const
    {
        std::unique_lock<std::mutex> guard(convertLock_);
        if (i420Buffer_)
            return i420Buffer_;

        // [realmview fork] PM #12 H12 minimal: replace upstream RTC_DCHECK (compiled out
        // in release) with real null guards. The gmb->ToI420() result was unchecked,
        // which is the SIGSEGV at +0x4ba258 in tombstone _22 (build e50bb065,
        // 2026-05-16 22:34:04). Do NOT cache a null into i420Buffer_; let the next
        // call retry.
        if (!video_frame || !video_frame->HasGpuMemoryBuffer()) return nullptr;
        auto gmb = video_frame->GetGpuMemoryBuffer();
        if (!gmb) return nullptr;
        auto converted = gmb->ToI420();
        if (!converted) return nullptr;
        i420Buffer_ = converted;
        return i420Buffer_;
    }

}
}
