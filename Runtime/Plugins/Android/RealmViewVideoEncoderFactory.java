// RealmViewVideoEncoderFactory.java
//
// [realmview fork] Subclass of libwebrtc's HardwareVideoEncoderFactory that wraps
// each created encoder with RealmViewVideoEncoder. The wrapper intercepts
// initEncode() and immediately requests Periodic Intra-Refresh (PIR) via
// MediaCodec.setParameters() to avoid the loss-amplification cycle that big
// periodic IDRs cause on lossy uplinks (Quest 3 hotspot).
//
// Why a subclass and not a separate VideoEncoderFactory: libwebrtc's
// HardwareVideoEncoderFactory contains MediaCodec capability detection logic
// (codec availability per device, H.264 profile filtering, etc.) that we don't
// want to reimplement. Subclassing keeps all of that intact and just wraps the
// final VideoEncoder result.
//
// Loaded by AndroidCodecFactoryHelper.cpp via JNI under the class name
// "com/qubenrealm/webrtc/RealmViewVideoEncoderFactory". Constructor signature
// MUST stay identical to the parent because the native code calls it via
// reflection with a fixed signature: (Lorg/webrtc/EglBase$Context;ZZ)V

package com.qubenrealm.webrtc;

import org.webrtc.EglBase;
import org.webrtc.HardwareVideoEncoderFactory;
import org.webrtc.VideoCodecInfo;
import org.webrtc.VideoEncoder;

public class RealmViewVideoEncoderFactory extends HardwareVideoEncoderFactory {

    private static final String TAG = "RealmViewVideoEncoderFactory";

    public RealmViewVideoEncoderFactory(
            EglBase.Context sharedContext,
            boolean enableIntelVp8Encoder,
            boolean enableH264HighProfile) {
        super(sharedContext, enableIntelVp8Encoder, enableH264HighProfile);
        android.util.Log.i(TAG, "RealmViewVideoEncoderFactory constructed (PIR-enabled fork)");
    }

    @Override
    public VideoEncoder createEncoder(VideoCodecInfo info) {
        VideoEncoder upstream = super.createEncoder(info);
        if (upstream == null) {
            // Either the codec is unsupported on this device or libwebrtc rejected
            // it during capability detection. Either way, return null to preserve
            // the upstream contract -- the caller falls back to a software encoder
            // or the next factory in the chain.
            android.util.Log.w(TAG, "Upstream factory returned null encoder for " + info.name + "; passing through");
            return null;
        }
        android.util.Log.i(TAG, "Wrapping " + info.name + " encoder with PIR injection");
        return new RealmViewVideoEncoder(upstream);
    }
}
