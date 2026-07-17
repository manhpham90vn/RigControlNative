package com.rigcontrol.server;

import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaCodec;
import android.media.MediaFormat;
import android.os.Build;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.ByteBuffer;

/**
 * Capture audio phát ra của thiết bị (nguồn REMOTE_SUBMIX, cần Android 11+/API 30), encode Opus
 * bằng MediaCodec, đóng gói theo audio_packet (docs/PROTOCOL.md §3b — cùng khung với video) và
 * ghi ra audio socket.
 *
 * Chạy từ app_process (uid shell) nên phải {@link Workarounds#fillAppInfo()} +
 * {@link Workarounds#prepareMainLooper()} trước khi dựng {@link AudioRecord}.
 *
 * Nếu thiết bị/encoder không hỗ trợ → {@link #prepare()} ném lỗi, Server gửi ACODEC_ID_NONE và
 * tiếp tục video bình thường (socket audio vẫn giữ mở).
 */
public final class AudioEncoder {
    // MediaRecorder.AudioSource.REMOTE_SUBMIX (hidden = 8); dùng literal để không phụ thuộc SDK ẩn.
    private static final int SOURCE_REMOTE_SUBMIX = 8;
    private static final int SAMPLE_RATE = 48000;
    private static final int CHANNELS = 2;
    private static final int BYTES_PER_FRAME = CHANNELS * 2; // PCM 16-bit
    private static final int AUDIO_BIT_RATE = 128_000;
    private static final long DEQUEUE_TIMEOUT_US = 5_000;

    private AudioRecord recorder;
    private MediaCodec codec;

    /** true nếu nền tảng có thể capture audio phát ra (Android 11+). */
    public boolean isAvailable() {
        return Build.VERSION.SDK_INT >= Build.VERSION_CODES.R; // API 30
    }

    public int codecId() {
        // Phase 1 chỉ hiện thực Opus; các codec khác để phase sau.
        return Protocol.ACODEC_ID_OPUS;
    }

    public int sampleRate() {
        return SAMPLE_RATE;
    }
    public int channels() {
        return CHANNELS;
    }

    /**
     * Khởi tạo capture + encoder. Phải gọi trên chính thread sẽ chạy {@link #streamTo}
     * (AudioRecord cần Looper trên thread khởi tạo). Ném lỗi nếu không khả dụng.
     */
    public void prepare() throws IOException {
        Workarounds.prepareMainLooper();
        Workarounds.fillAppInfo();

        recorder = buildRecorder();
        if (recorder.getState() != AudioRecord.STATE_INITIALIZED) {
            recorder.release();
            recorder = null;
            throw new IOException("AudioRecord không khởi tạo được (REMOTE_SUBMIX)");
        }

        try {
            codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_OPUS);
            codec.configure(createFormat(), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        } catch (Exception e) {
            release();
            throw new IOException("không tạo được encoder Opus", e);
        }
    }

    private AudioRecord buildRecorder() {
        AudioFormat format = new AudioFormat.Builder()
                                 .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                                 .setSampleRate(SAMPLE_RATE)
                                 .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
                                 .build();
        int minBuf = AudioRecord.getMinBufferSize(
            SAMPLE_RATE, AudioFormat.CHANNEL_IN_STEREO, AudioFormat.ENCODING_PCM_16BIT);
        if (minBuf <= 0) {
            minBuf = SAMPLE_RATE * BYTES_PER_FRAME / 10; // ~100ms fallback
        }
        return new AudioRecord.Builder()
            .setAudioSource(SOURCE_REMOTE_SUBMIX)
            .setAudioFormat(format)
            .setBufferSizeInBytes(minBuf * 4)
            .build();
    }

    private MediaFormat createFormat() {
        MediaFormat format = new MediaFormat();
        format.setString(MediaFormat.KEY_MIME, MediaFormat.MIMETYPE_AUDIO_OPUS);
        format.setInteger(MediaFormat.KEY_SAMPLE_RATE, SAMPLE_RATE);
        format.setInteger(MediaFormat.KEY_CHANNEL_COUNT, CHANNELS);
        format.setInteger(MediaFormat.KEY_BIT_RATE, AUDIO_BIT_RATE);
        return format;
    }

    /** Vòng capture → encode → ghi ra out. Gọi sau {@link #prepare()}. */
    public void streamTo(OutputStream out) throws IOException {
        codec.start();
        startRecordingWithRetry();

        MediaCodec.BufferInfo bufferInfo = new MediaCodec.BufferInfo();
        byte[] payload = new byte[ScreenEncoder.PACKET_HDR + 16 * 1024];
        long totalBytes = 0;
        try {
            while (true) {
                int inIndex = codec.dequeueInputBuffer(DEQUEUE_TIMEOUT_US);
                if (inIndex >= 0) {
                    ByteBuffer in = codec.getInputBuffer(inIndex);
                    in.clear();
                    int read = recorder.read(in, in.capacity());
                    long ptsUs = 1_000_000L * (totalBytes / BYTES_PER_FRAME) / SAMPLE_RATE;
                    if (read > 0) {
                        totalBytes += read;
                        codec.queueInputBuffer(inIndex, 0, read, ptsUs, 0);
                    } else {
                        codec.queueInputBuffer(inIndex, 0, 0, ptsUs, 0);
                    }
                }

                int outIndex;
                while (
                    (outIndex = codec.dequeueOutputBuffer(bufferInfo, DEQUEUE_TIMEOUT_US)) >= 0) {
                    try {
                        if (bufferInfo.size > 0
                            && (bufferInfo.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) == 0) {
                            ByteBuffer bb = codec.getOutputBuffer(outIndex);
                            bb.position(bufferInfo.offset);
                            bb.limit(bufferInfo.offset + bufferInfo.size);
                            if (payload.length < ScreenEncoder.PACKET_HDR + bufferInfo.size) {
                                payload = new byte[ScreenEncoder.PACKET_HDR + bufferInfo.size];
                            }
                            bb.get(payload, ScreenEncoder.PACKET_HDR, bufferInfo.size);
                            boolean config =
                                (bufferInfo.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0;
                            // Audio dùng chung khung với video; không có "keyframe".
                            ScreenEncoder.writePacket(out, config, false,
                                bufferInfo.presentationTimeUs, payload, bufferInfo.size);
                        }
                    } finally {
                        codec.releaseOutputBuffer(outIndex, false);
                    }
                }
            }
        } finally {
            release();
        }
    }

    private void startRecordingWithRetry() throws IOException {
        for (int attempt = 0; attempt < 3; attempt++) {
            recorder.startRecording();
            if (recorder.getRecordingState() == AudioRecord.RECORDSTATE_RECORDING) {
                return;
            }
            try {
                Thread.sleep(50);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                break;
            }
        }
        throw new IOException("AudioRecord.startRecording thất bại");
    }

    public void release() {
        if (recorder != null) {
            try {
                recorder.stop();
            } catch (Exception ignored) {
            }
            recorder.release();
            recorder = null;
        }
        if (codec != null) {
            try {
                codec.stop();
            } catch (Exception ignored) {
            }
            codec.release();
            codec = null;
        }
    }
}
