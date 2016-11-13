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
 * 包含必要的头文件
 * */
#include <assert.h>
//jni代码
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
//有关media的
#include "media/NdkMediaCodec.h"
#include "media/NdkMediaExtractor.h"


// for __android_log_print(ANDROID_LOG_INFO, "YourApp", "formatted message");
//有关调试信息的log
#include <android/log.h>

//LOG  标签
#define TAG "NativeCodec"
#define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, TAG, __VA_ARGS__)

// for native window JNI
#include <android/native_window_jni.h>

//解码工作状态结构体
typedef struct {
	//fd 文件描述符
    int fd;
    ANativeWindow* window;
    AMediaExtractor* ex;
    AMediaCodec *codec;
    int64_t renderstart;
    //状态标识位
    bool sawInputEOS;
    bool sawOutputEOS;
    bool isPlaying;
    bool renderonce;
} workerdata;

//工作数据
workerdata data = {-1, NULL, NULL, NULL, 0, false, false, false, false};


//codec枚举消息类型
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

//指向mlooper
static mylooper *mlooper = NULL;

//系统时间
int64_t systemnanotime() {
	//当前时间
    timespec now;
    //获取大年时间
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000000000LL + now.tv_nsec;
}


//回调函数
void doCodecWork(workerdata *d) {

	//
    ssize_t bufidx = -1;
    if (!d->sawInputEOS) {
    	//输入源不为空
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

//looper处理函数
void mylooper::handle(int what, void* obj) {
    switch (what) {
    	//解码缓存
        case kMsgCodecBuffer:
            doCodecWork((workerdata*)obj);
            break;

        //解码完毕
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
        //解码查询
        case kMsgSeek:
        {
            workerdata *d = (workerdata*)obj;
            //
            AMediaExtractor_seekTo(d->ex, 0, AMEDIAEXTRACTOR_SEEK_NEXT_SYNC);
            //刷新视频
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

        //暂停播放
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

        //恢复播放
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



//说明是C函数
extern "C" {

//创建一个码流播放器
jboolean Java_com_example_nativecodec_NativeCodec_createStreamingMediaPlayer(JNIEnv* env,
        jclass clazz, jstring filename)
{
    LOGV("@@@ create");

    // convert Java string to UTF-8
    //将java的字符串转换成c字符串
    const char *utf8 = env->GetStringUTFChars(filename, NULL);
    LOGV("opening %s", utf8);
    int fd = open(utf8, O_RDONLY);
    //在此要释放字符转换
    env->ReleaseStringUTFChars(filename, utf8);
    if (fd < 0) {
        LOGV("failed: %d (%s)", fd, strerror(errno));
        return JNI_FALSE;
    }

    //视频文件的文件描述符
    data.fd = fd;

    workerdata *d = &data;

    //创建一个混合多媒体提取器结构体  这个提取器可以提取容器的音频和视频
    AMediaExtractor *ex = AMediaExtractor_new();
    media_status_t err = AMediaExtractor_setDataSourceFd(ex, d->fd, 0 , LONG_MAX);
    close(d->fd);
    if (err != AMEDIA_OK) {
        LOGV("setDataSource error: %d", err);
        return JNI_FALSE;
    }

    //确认这个多媒体提取器中可以分离出几个部分，一般1个或2个，1个音频或视频，或都包含
    int numtracks = AMediaExtractor_getTrackCount(ex);

    AMediaCodec *codec = NULL;

    //将一个视频文件分成两个部分
    //一个是视频 =0 ，一个音频 = 1.
    //所以这部分tracks = 2
    LOGV("input has %d tracks", numtracks);
    for (int i = 0; i < numtracks; i++) {
    	//1.获取视频容器中视频的格式信息
        AMediaFormat *format = AMediaExtractor_getTrackFormat(ex, i);
        //这部分是提取了视频的信息
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
        	//确实是支持video视频格式文件的
            // Omitting most error handling for clarity.
            // Production code should check for errors.
            AMediaExtractor_selectTrack(ex, i);
            //创建一个解码的decode结构体,他是用于解码264或其他视频文件的
            codec = AMediaCodec_createDecoderByType(mime);
            //配置多媒体codec
            AMediaCodec_configure(codec, format, d->window, NULL, 0);
            //多媒体解析器
            d->ex = ex;
            d->codec = codec;//多媒体解码器
            d->renderstart = -1;
            d->sawInputEOS = false;
            d->sawOutputEOS = false;
            d->isPlaying = false;
            d->renderonce = true;
            //开始转换  包括视频和音频
            AMediaCodec_start(codec);
        }
        //将媒体格式删除
        AMediaFormat_delete(format);
    }

    //循环looper结构体初始化
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
//关闭native的多媒体系统
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
//设置suface
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
