// RealmViewVideoEncoder.java
//
// [realmview fork] VideoEncoder delegating wrapper that intercepts initEncode()
// to inject Periodic Intra-Refresh (PIR) configuration into the underlying
// HardwareVideoEncoder's MediaCodec instance.
//
// Implementation strategy:
//   1. Delegate every VideoEncoder method to the wrapped instance unchanged.
//   2. On initEncode() success, reflect into the wrapped HardwareVideoEncoder's
//      private `mediaCodec` field and call MediaCodec.setParameters(...) with
//      our intra-refresh + bitrate-mode keys.
//
// Reflection rationale: libwebrtc's HardwareVideoEncoder builds its MediaFormat
// internally before calling MediaCodec.configure(). We cannot intercept the
// MediaFormat at configure() time without rebuilding libwebrtc (the "Path B"
// alternative in webrtc_fork.md). The remaining knob is MediaCodec.setParameters()
// which works on an already-running encoder via a Bundle interface.
//
// KEY POINTS OF UNCERTAINTY (verify via Quest 3 logs after first build):
//   - Quest 3's MediaCodec vendor may ignore "intra-refresh-period" set via
//     setParameters() and only honour it on the initial MediaFormat. If so, we
//     escalate to Path B (rebuild libwebrtc to add the key to the MediaFormat).
//   - Some Android vendors require KEY_BITRATE_MODE = CBR for PIR to work. We
//     try to set both via setParameters() but the vendor may reject the bitrate
//     mode change post-configure().
//   - The HardwareVideoEncoder.mediaCodec field name is libwebrtc-internal and
//     could be renamed in a future libwebrtc M-version. We pin to M116 via the
//     fork; any upstream sync needs to re-verify the field name.

package com.qubenrealm.webrtc;

import android.media.MediaCodec;
import android.media.MediaFormat;
import android.os.Bundle;
import android.util.Log;

import org.webrtc.EncodedImage;
import org.webrtc.VideoCodecStatus;
import org.webrtc.VideoEncoder;
import org.webrtc.VideoFrame;

import java.lang.reflect.Field;

public class RealmViewVideoEncoder implements VideoEncoder {

    private static final String TAG = "RealmViewVideoEncoder";

    // PIR period in FRAMES. At 30 fps this gives a ~2 s refresh cycle, which is
    // the recommended cadence for archive recording per webrtc_changelog.md.
    // Configurable via setIntraRefreshPeriodFrames() if we want to expose it to
    // C# later -- for now it's a hard-coded sensible default.
    private static final int INTRA_REFRESH_PERIOD_FRAMES = 60;

    // From MediaFormat docs: BITRATE_MODE_CBR = 2 (constant bitrate, no overshoot).
    // Several Android H.264 encoder vendors silently disable intra-refresh unless
    // bitrate mode is CBR. We try to set this defensively.
    private static final int BITRATE_MODE_CBR = 2;

    private final VideoEncoder wrapped;
    private boolean pirInjected = false;

    public RealmViewVideoEncoder(VideoEncoder wrapped) {
        this.wrapped = wrapped;
    }

    // ---------- VideoEncoder interface delegation ----------

    @Override
    public VideoCodecStatus initEncode(Settings settings, Callback callback) {
        VideoCodecStatus status = wrapped.initEncode(settings, callback);
        if (status == VideoCodecStatus.OK) {
            // Encoder is now configured and running. Inject our parameters.
            // Best-effort: a failure here doesn't break encoding, just means
            // PIR doesn't take effect and we fall back to upstream behaviour.
            injectIntraRefreshParameters();
        } else {
            Log.w(TAG, "Wrapped initEncode failed with status " + status + "; skipping PIR injection");
        }
        return status;
    }

    @Override
    public VideoCodecStatus release() {
        pirInjected = false;
        return wrapped.release();
    }

    @Override
    public VideoCodecStatus encode(VideoFrame frame, EncodeInfo info) {
        return wrapped.encode(frame, info);
    }

    @Override
    public VideoCodecStatus setRateAllocation(BitrateAllocation allocation, int framerate) {
        return wrapped.setRateAllocation(allocation, framerate);
    }

    @Override
    public VideoCodecStatus setRates(RateControlParameters rcParameters) {
        return wrapped.setRates(rcParameters);
    }

    @Override
    public ScalingSettings getScalingSettings() {
        return wrapped.getScalingSettings();
    }

    @Override
    public ResolutionBitrateLimits[] getResolutionBitrateLimits() {
        return wrapped.getResolutionBitrateLimits();
    }

    @Override
    public String getImplementationName() {
        return "RealmView/" + wrapped.getImplementationName();
    }

    @Override
    public EncoderInfo getEncoderInfo() {
        return wrapped.getEncoderInfo();
    }

    // ---------- PIR injection ----------

    private void injectIntraRefreshParameters() {
        if (pirInjected) return;
        try {
            MediaCodec mediaCodec = extractMediaCodec(wrapped);
            if (mediaCodec == null) {
                Log.w(TAG, "Could not locate MediaCodec in wrapped encoder; PIR not applied. "
                        + "Encoder will run with upstream defaults (effectively infinite GOP on Quest).");
                return;
            }

            Bundle params = new Bundle();
            params.putInt(MediaFormat.KEY_INTRA_REFRESH_PERIOD, INTRA_REFRESH_PERIOD_FRAMES);
            params.putInt(MediaFormat.KEY_BITRATE_MODE, BITRATE_MODE_CBR);
            mediaCodec.setParameters(params);

            // Read back the format the encoder is actually using. If the vendor
            // silently clamped intra-refresh-period to 0, we want loud evidence
            // of that in the log so the operator knows PIR didn't take.
            try {
                MediaFormat outFormat = mediaCodec.getOutputFormat();
                int actualPir = outFormat.containsKey(MediaFormat.KEY_INTRA_REFRESH_PERIOD)
                        ? outFormat.getInteger(MediaFormat.KEY_INTRA_REFRESH_PERIOD)
                        : -1;
                Log.i(TAG, "PIR injected: requested=" + INTRA_REFRESH_PERIOD_FRAMES
                        + " frames, output-format reports=" + actualPir
                        + " (-1 = key absent, which on some vendors is normal even when PIR is active)");
            } catch (Exception readBackEx) {
                Log.w(TAG, "getOutputFormat() failed during PIR verification: " + readBackEx.getMessage()
                        + " (encoder is still running; verification is best-effort)");
            }

            pirInjected = true;
            Log.i(TAG, "PIR injection successful. Period=" + INTRA_REFRESH_PERIOD_FRAMES
                    + " frames, BitrateMode=CBR");
        } catch (Exception e) {
            // Catch-all: any reflection or MediaCodec exception here must NOT
            // break encoding. Log loudly and let the encoder continue with
            // upstream defaults.
            Log.e(TAG, "PIR injection failed; encoder running with upstream defaults: " + e.getMessage(), e);
        }
    }

    /**
     * Reflect into the libwebrtc HardwareVideoEncoder to find its private
     * MediaCodec field. The field name `mediaCodec` is stable in libwebrtc M116
     * (verified against the M116-20250805 webrtc-android.zip release that this
     * fork bundles). Upstream sync to a newer libwebrtc M-version must re-verify
     * this name and update if changed.
     */
    private static MediaCodec extractMediaCodec(VideoEncoder encoder) {
        // The wrapped encoder may be HardwareVideoEncoder directly OR a further
        // libwebrtc-internal wrapper (e.g. a fallback decorator). Walk up the
        // class hierarchy looking for the mediaCodec field.
        Class<?> cls = encoder.getClass();
        while (cls != null && cls != Object.class) {
            try {
                Field f = cls.getDeclaredField("mediaCodec");
                f.setAccessible(true);
                Object value = f.get(encoder);
                if (value instanceof MediaCodec) {
                    return (MediaCodec) value;
                }
            } catch (NoSuchFieldException ignored) {
                // Try parent class.
            } catch (IllegalAccessException e) {
                Log.w(TAG, "Reflection access denied for " + cls.getName() + ".mediaCodec: " + e.getMessage());
                return null;
            }
            cls = cls.getSuperclass();
        }
        return null;
    }
}
