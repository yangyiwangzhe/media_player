/*
 * Copyright (C) 2014 The Android Open Source Project
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

/* This is a JNI example where we use native methods to play video
 * using the native AMedia* APIs.
 * See the corresponding Java source file located at:
 *
 *   src/com/example/nativecodec/NativeMedia.java
 *
 * In this example we use assert() for "impossible" error conditions,
 * and explicit handling and recovery for more likely error conditions.
 */

/*
 * ������Ҫ��ͷ�ļ�
 * */
#include <assert.h>
//jni����
#include <jni.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>

#include "looper.h"
//�й�media��
#include "media/NdkMediaCodec.h"
#include "media/NdkMediaExtractor.h"


// for __android_log_print(ANDROID_LOG_INFO, "YourApp", "formatted message");
//�йص�����Ϣ��log
#include <android/log.h>

//LOG  ��ǩ
#define TAG "NativeCodec"
#define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, TAG, __VA_ARGS__)

// for native window JNI
#include <android/native_window_jni.h>

//���빤��״̬�ṹ��
typedef struct {
	//fd �ļ�������
    int fd;
    ANativeWindow* window;
    AMediaExtractor* ex;
    AMediaCodec *codec;
    int64_t renderstart;
    //״̬��ʶλ
    bool sawInputEOS;
    bool sawOutputEOS;
    bool isPlaying;
    bool renderonce;
} workerdata;

//��������
workerdata data = {-1, NULL, NULL, NULL, 0, false, false, false, false};


//codecö����Ϣ����
enum {
    kMsgCodecBuffer,
    kMsgPause,
    kMsgResume,
    kMsgPauseAck,
    kMsgDecodeDone,
    kMsgSeek,
};

//
class mylooper: public looper {
    virtual void handle(int what, void* obj);
};

//ָ��mlooper
static mylooper *mlooper = NULL;

//ϵͳʱ��
int64_t systemnanotime() {
	//��ǰʱ��
    timespec now;
    //��ȡ����ʱ��
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000000000LL + now.tv_nsec;
}


//�ص�����
void doCodecWork(workerdata *d) {

	//
    ssize_t bufidx = -1;
    if (!d->sawInputEOS) {
    	//����Դ��Ϊ��
        bufidx = AMediaCodec_dequeueInputBuffer(d->codec, 2000);
        LOGV("input buffer %zd", bufidx);
        if (bufidx >= 0) {
            size_t bufsize;
            uint8_t *buf = AMediaCodec_getInputBuffer(d->codec, bufidx, &bufsize);
            ssize_t sampleSize = AMediaExtractor_readSampleData(d->ex, buf, bufsize);
            if (sampleSize < 0) {
                sampleSize = 0;
                d->sawInputEOS = true;
                LOGV("EOS");
            }
            int64_t presentationTimeUs = AMediaExtractor_getSampleTime(d->ex);

            AMediaCodec_queueInputBuffer(d->codec, bufidx, 0, sampleSize, presentationTimeUs,
                    d->sawInputEOS ? AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM : 0);
            AMediaExtractor_advance(d->ex);
        }
    }

    //
    if (!d->sawOutputEOS) {
        AMediaCodecBufferInfo info;
        ssize_t status = AMediaCodec_dequeueOutputBuffer(d->codec, &info, 0);
        if (status >= 0) {
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
                LOGV("output EOS");
                d->sawOutputEOS = true;
            }
            int64_t presentationNano = info.presentationTimeUs * 1000;
            if (d->renderstart < 0) {
                d->renderstart = systemnanotime() - presentationNano;
            }
            int64_t delay = (d->renderstart + presentationNano) - systemnanotime();
            if (delay > 0) {
                usleep(delay / 1000);
            }
            AMediaCodec_releaseOutputBuffer(d->codec, status, info.size != 0);
            if (d->renderonce) {
                d->renderonce = false;
                return;
            }
        } else if (status == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            LOGV("output buffers changed");
        } else if (status == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *format = NULL;
            format = AMediaCodec_getOutputFormat(d->codec);
            LOGV("format changed to: %s", AMediaFormat_toString(format));
            AMediaFormat_delete(format);
        } else if (status == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            LOGV("no output buffer right now");
        } else {
            LOGV("unexpected info code: %zd", status);
        }
    }

    if (!d->sawInputEOS || !d->sawOutputEOS) {
        mlooper->post(kMsgCodecBuffer, d);
    }
}

//looper������
void mylooper::handle(int what, void* obj) {
    switch (what) {
    	//���뻺��
        case kMsgCodecBuffer:
            doCodecWork((workerdata*)obj);
            break;

        //�������
        case kMsgDecodeDone:
        {
            workerdata *d = (workerdata*)obj;
            AMediaCodec_stop(d->codec);
            AMediaCodec_delete(d->codec);
            AMediaExtractor_delete(d->ex);
            d->sawInputEOS = true;
            d->sawOutputEOS = true;
        }
        break;
        //�����ѯ
        case kMsgSeek:
        {
            workerdata *d = (workerdata*)obj;
            //
            AMediaExtractor_seekTo(d->ex, 0, AMEDIAEXTRACTOR_SEEK_NEXT_SYNC);
            //ˢ����Ƶ
            AMediaCodec_flush(d->codec);
            d->renderstart = -1;
            d->sawInputEOS = false;
            d->sawOutputEOS = false;
            if (!d->isPlaying) {
                d->renderonce = true;
                post(kMsgCodecBuffer, d);
            }
            LOGV("seeked");
        }
        break;

        //��ͣ����
        case kMsgPause:
        {
            workerdata *d = (workerdata*)obj;
            if (d->isPlaying) {
                // flush all outstanding codecbuffer messages with a no-op message
                d->isPlaying = false;
                post(kMsgPauseAck, NULL, true);
            }
        }
        break;

        //�ָ�����
        case kMsgResume:
        {
            workerdata *d = (workerdata*)obj;
            if (!d->isPlaying) {
                d->renderstart = -1;
                d->isPlaying = true;
                post(kMsgCodecBuffer, d);
            }
        }
        break;
    }
}



//˵����C����
extern "C" {

//����һ������������
jboolean Java_com_example_nativecodec_NativeCodec_createStreamingMediaPlayer(JNIEnv* env,
        jclass clazz, jstring filename)
{
    LOGV("@@@ create");

    // convert Java string to UTF-8
    //��java���ַ���ת����c�ַ���
    const char *utf8 = env->GetStringUTFChars(filename, NULL);
    LOGV("opening %s", utf8);
    int fd = open(utf8, O_RDONLY);
    //�ڴ�Ҫ�ͷ��ַ�ת��
    env->ReleaseStringUTFChars(filename, utf8);
    if (fd < 0) {
        LOGV("failed: %d (%s)", fd, strerror(errno));
        return JNI_FALSE;
    }

    //��Ƶ�ļ����ļ�������
    data.fd = fd;

    workerdata *d = &data;

    //����һ����϶�ý����ȡ���ṹ��  �����ȡ��������ȡ��������Ƶ����Ƶ
    AMediaExtractor *ex = AMediaExtractor_new();
    media_status_t err = AMediaExtractor_setDataSourceFd(ex, d->fd, 0 , LONG_MAX);
    close(d->fd);
    if (err != AMEDIA_OK) {
        LOGV("setDataSource error: %d", err);
        return JNI_FALSE;
    }

    //ȷ�������ý����ȡ���п��Է�����������֣�һ��1����2����1����Ƶ����Ƶ���򶼰���
    int numtracks = AMediaExtractor_getTrackCount(ex);

    AMediaCodec *codec = NULL;

    //��һ����Ƶ�ļ��ֳ���������
    //һ������Ƶ =0 ��һ����Ƶ = 1.
    //�����ⲿ��tracks = 2
    LOGV("input has %d tracks", numtracks);
    for (int i = 0; i < numtracks; i++) {
    	//1.��ȡ��Ƶ��������Ƶ�ĸ�ʽ��Ϣ
        AMediaFormat *format = AMediaExtractor_getTrackFormat(ex, i);
        //�ⲿ������ȡ����Ƶ����Ϣ
        /*
         *  track 0 format: mime: string(video/avc), durationUs: int64(10000000),
         *  width: int32(480), height: int32(360),
         *  rotation-degrees: int32(0),
         *  max-input-size: int32(230401),
         *  csd-0: data, csd-1: data}
         * */
        const char *s = AMediaFormat_toString(format);
        LOGV("track %d format: %s", i, s);
        //
        const char *mime;
        if (!AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &mime)) {
            LOGV("no mime type");
            return JNI_FALSE;
        } else if (!strncmp(mime, "video/", 6)) {
        	//ȷʵ��֧��video��Ƶ��ʽ�ļ���
            // Omitting most error handling for clarity.
            // Production code should check for errors.
            AMediaExtractor_selectTrack(ex, i);
            //����һ�������decode�ṹ��,�������ڽ���264��������Ƶ�ļ���
            codec = AMediaCodec_createDecoderByType(mime);
            //���ö�ý��codec
            AMediaCodec_configure(codec, format, d->window, NULL, 0);
            //��ý�������
            d->ex = ex;
            d->codec = codec;//��ý�������
            d->renderstart = -1;
            d->sawInputEOS = false;
            d->sawOutputEOS = false;
            d->isPlaying = false;
            d->renderonce = true;
            //��ʼת��  ������Ƶ����Ƶ
            AMediaCodec_start(codec);
        }
        //��ý���ʽɾ��
        AMediaFormat_delete(format);
    }

    //ѭ��looper�ṹ���ʼ��
    mlooper = new mylooper();
    //
    mlooper->post(kMsgCodecBuffer, d);

    return JNI_TRUE;
}

// set the playing state for the streaming media player
//
void Java_com_example_nativecodec_NativeCodec_setPlayingStreamingMediaPlayer(JNIEnv* env,
        jclass clazz, jboolean isPlaying)
{
    LOGV("@@@ playpause: %d", isPlaying);
    if (mlooper) {
        if (isPlaying) {
            mlooper->post(kMsgResume, &data);
        } else {
            mlooper->post(kMsgPause, &data);
        }
    }
}


// shut down the native media system
//�ر�native�Ķ�ý��ϵͳ
void Java_com_example_nativecodec_NativeCodec_shutdown(JNIEnv* env, jclass clazz)
{
    LOGV("@@@ shutdown");
    if (mlooper) {
        mlooper->post(kMsgDecodeDone, &data, true /* flush */);
        mlooper->quit();
        delete mlooper;
        mlooper = NULL;
    }
    if (data.window) {
        ANativeWindow_release(data.window);
        data.window = NULL;
    }
}


// set the surface
//����suface
void Java_com_example_nativecodec_NativeCodec_setSurface(JNIEnv *env, jclass clazz, jobject surface)
{
    // obtain a native window from a Java surface
    if (data.window) {
        ANativeWindow_release(data.window);
        data.window = NULL;
    }
    data.window = ANativeWindow_fromSurface(env, surface);
    LOGV("@@@ setsurface %p", data.window);
}


// rewind the streaming media player
void Java_com_example_nativecodec_NativeCodec_rewindStreamingMediaPlayer(JNIEnv *env, jclass clazz)
{
    LOGV("@@@ rewind");
    mlooper->post(kMsgSeek, &data);
}

}
