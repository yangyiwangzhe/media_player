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
//线程和信号量结构体
#include <pthread.h>
#include <semaphore.h>

//
struct loopermessage;


/*looper结构体申请*/
class looper {
    public:
		//构造函数
        looper();
        //析构函数
        ~looper();

        void post(int what, void *data, bool flush = false);
        void quit();

        virtual void handle(int what, void *data);

    private:
        //添加信号
        void addmsg(loopermessage *msg, bool flush);
        static void* trampoline(void* p);
        //构造函数
        void loop();
        loopermessage *head;
        pthread_t worker;
        //信号量定义
        sem_t headwriteprotect;
        sem_t headdataavailable;
        //
        bool running;
};
