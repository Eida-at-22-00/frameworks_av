/*
 * Copyright (C) 2024 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package com.android.media.benchmark.library;

import android.media.MediaCodec;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.os.Build;
import android.util.Log;

import androidx.annotation.NonNull;

import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.List;

import com.android.media.benchmark.library.Decoder;

public class BlockModelDecoder extends Decoder {
    private static final String TAG = BlockModelDecoder.class.getSimpleName();
    private final boolean DEBUG = false;
    protected final LinearBlockWrapper mLinearInputBlock = new LinearBlockWrapper();

    /**
     * Wrapper class for {@link MediaCodec.LinearBlock}
     */
    public static class LinearBlockWrapper {
        private MediaCodec.LinearBlock mBlock;
        private ByteBuffer mBuffer;
        private int mOffset;

        public MediaCodec.LinearBlock getBlock() {
            return mBlock;
        }

        public ByteBuffer getBuffer() {
            return mBuffer;
        }

        public int getBufferCapacity() {
            return mBuffer == null ? 0 : mBuffer.capacity();
        }

        public int getOffset() {
            return mOffset;
        }

        public void setOffset(int size) {
            mOffset = size;
        }

        public boolean allocateBlock(String codec, int size) throws RuntimeException{
            recycle();
            mBlock = MediaCodec.LinearBlock.obtain(size, new String[]{codec});
            if (mBlock == null || !mBlock.isMappable()) {
                throw new RuntimeException("Linear Block not allocated/mapped");
            }
            mBuffer = mBlock.map();
            mOffset = 0;
            return true;
        }

        public void recycle() {
            if (mBlock != null) {
                mBlock.recycle();
                mBlock = null;
            }
            mBuffer = null;
            mOffset = 0;
        }
    }

    public BlockModelDecoder() {
        // empty
    }

    public void tearDown() {
        mLinearInputBlock.recycle();

    }

    /**
     * Decodes the given input buffer,
     * provided valid list of buffer info and format are passed as inputs.
     *
     * @param inputBuffer     Decode the provided list of ByteBuffers
     * @param inputBufferInfo List of buffer info corresponding to provided input buffers
     * @param asyncMode       Will run on async implementation if true
     * @param format          For creating the decoder if codec name is empty and configuring it
     * @param codecName       Will create the decoder with codecName
     * @return DECODE_SUCCESS if decode was successful, DECODE_DECODER_ERROR for fail,
     *         DECODE_CREATE_ERROR for decoder not created
     * @throws IOException if the codec cannot be created.
     */
    @Override
    public int decode(@NonNull List<ByteBuffer> inputBuffer,
        @NonNull List<MediaCodec.BufferInfo> inputBufferInfo, final boolean asyncMode,
        @NonNull MediaFormat format, String codecName)
        throws IOException, InterruptedException {
        setExtraConfigureFlags(MediaCodec.CONFIGURE_FLAG_USE_BLOCK_MODEL);
        return super.decode(inputBuffer, inputBufferInfo, asyncMode, format, codecName);
    }

    @Override
    protected void onInputAvailable(int inputBufferId, MediaCodec mediaCodec) {
        if (mNumInFramesProvided >= mNumInFramesRequired) {
            mIndex = mInputBufferInfo.size() - 1;
        }
        MediaCodec.BufferInfo bufInfo = mInputBufferInfo.get(mIndex);
        mSawInputEOS = (bufInfo.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
        if (mLinearInputBlock.getOffset() + bufInfo.size > mLinearInputBlock.getBufferCapacity()) {
            int requestSize = 8192;
            requestSize = Math.max(bufInfo.size, requestSize);
            mLinearInputBlock.allocateBlock(mediaCodec.getCanonicalName(), requestSize);
        }
        int codecFlags = 0;
        if ((bufInfo.flags & MediaExtractor.SAMPLE_FLAG_SYNC) != 0) {
            codecFlags |= MediaCodec.BUFFER_FLAG_KEY_FRAME;
        }
        if ((bufInfo.flags & MediaExtractor.SAMPLE_FLAG_PARTIAL_FRAME) != 0) {
            codecFlags |= MediaCodec.BUFFER_FLAG_PARTIAL_FRAME;
        }
        codecFlags |= mSawInputEOS ? MediaCodec.BUFFER_FLAG_END_OF_STREAM : 0;
        if (DEBUG) {
            Log.v(TAG, "input: id: " + inputBufferId
                    + " size: " + bufInfo.size
                    + " pts: " + bufInfo.presentationTimeUs
                    + " flags: " + codecFlags);
        }
        mLinearInputBlock.getBuffer().put(mInputBuffer.get(mIndex).array());
        mNumInFramesProvided++;
        mIndex = mNumInFramesProvided % (mInputBufferInfo.size() - 1);
        if (mSawInputEOS) {
            Log.i(TAG, "Saw Input EOS");
        }
        mStats.addFrameSize(bufInfo.size);
        MediaCodec.QueueRequest request = mCodec.getQueueRequest(inputBufferId);
        request.setLinearBlock(mLinearInputBlock.getBlock(), mLinearInputBlock.getOffset(),
                bufInfo.size);
        request.setPresentationTimeUs(bufInfo.presentationTimeUs);
        request.setFlags(codecFlags);
        request.queue();
        if (bufInfo.size > 0 && (codecFlags & (MediaCodec.BUFFER_FLAG_CODEC_CONFIG
                | MediaCodec.BUFFER_FLAG_PARTIAL_FRAME)) == 0) {
            mLinearInputBlock.setOffset(mLinearInputBlock.getOffset() + bufInfo.size);
        }
    }

    @Override
    protected void onOutputAvailable(
            MediaCodec mediaCodec, int outputBufferId, MediaCodec.BufferInfo outputBufferInfo) {
        if (mSawOutputEOS || outputBufferId < 0) {
            return;
        }
        mNumOutputFrame++;
        if (DEBUG) {
            Log.d(TAG,
                    "In OutputBufferAvailable ,"
                            + " output frame number = " + mNumOutputFrame
                            + " timestamp = " + outputBufferInfo.presentationTimeUs
                            + " size = " + outputBufferInfo.size);
        }
        MediaCodec.OutputFrame outFrame = mediaCodec.getOutputFrame(outputBufferId);
        ByteBuffer outputBuffer = null;
        try {
            if (outFrame.getLinearBlock() != null) {
                outputBuffer = outFrame.getLinearBlock().map();
            }
        } catch(IllegalStateException e) {
            // buffer may not be linear, this is ok
            // as we are handling non-linear buffers below.
        }
        if (mOutputStream != null) {
            try {
                if (outputBuffer != null) {
                    byte[] bytesOutput = new byte[outputBuffer.remaining()];
                    outputBuffer.get(bytesOutput);
                    mOutputStream.write(bytesOutput);
                }
            } catch (IOException e) {
                e.printStackTrace();
                Log.d(TAG, "Error Dumping File: Exception " + e.toString());
            }
        }
        ByteBuffer copiedBuffer = null;
        int bytesRemaining = 0;
        if (outputBuffer != null) {
            bytesRemaining = outputBuffer.remaining();
            if (mIBufferSend != null) {
                copiedBuffer = ByteBuffer.allocate(outputBuffer.remaining());
                copiedBuffer.put(outputBuffer);
            }
            outFrame.getLinearBlock().recycle();
            outputBuffer = null;
        }
        if (mFrameReleaseQueue != null) {
            if (mMime.startsWith("audio/")) {
                try {
                    mFrameReleaseQueue.pushFrame(outputBufferId, bytesRemaining);
                } catch (Exception e) {
                    Log.d(TAG, "Error in getting MediaCodec buffer" + e.toString());
                }
            } else {
                mFrameReleaseQueue.pushFrame(mNumOutputFrame, outputBufferId,
                                                outputBufferInfo.presentationTimeUs);
            }

        } else if (mIBufferSend != null) {
            IBufferXfer.BufferXferInfo info = new IBufferXfer.BufferXferInfo();
            // TODO: may be inefficient;
            info.buf = copiedBuffer;
            info.idx = outputBufferId;
            info.obj = mediaCodec;
            info.bytesRead = outputBufferInfo.size;
            info.presentationTimeUs = outputBufferInfo.presentationTimeUs;
            info.flag = outputBufferInfo.flags;
            mIBufferSend.sendBuffer(this, info);
        } else {
            mediaCodec.releaseOutputBuffer(outputBufferId, mRender);
        }
        mSawOutputEOS = (outputBufferInfo.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
        if (DEBUG && mSawOutputEOS) {
            Log.i(TAG, "Saw output EOS");
        }
    }

}