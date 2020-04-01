/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "TimerQuery.h"

#include "private/backend/OpenGLPlatform.h"

#include <utils/compiler.h>
#include <utils/Log.h>
#include <utils/Systrace.h>

namespace filament {

using namespace backend;
using namespace GLUtils;

// ------------------------------------------------------------------------------------------------

TimerQueryInterface::~TimerQueryInterface() = default;

// ------------------------------------------------------------------------------------------------

TimerQueryNative::TimerQueryNative(OpenGLContext& context)
        : gl(context) {
}

TimerQueryNative::~TimerQueryNative() = default;

void TimerQueryNative::beginTimeElapsedQuery(GLTimerQuery* query) {
    gl.beginQuery(GL_TIME_ELAPSED, query->gl.query);
    CHECK_GL_ERROR(utils::slog.e)
}

void TimerQueryNative::endTimeElapsedQuery(GLTimerQuery* query) {
    gl.endQuery(GL_TIME_ELAPSED);
    CHECK_GL_ERROR(utils::slog.e)
}

bool TimerQueryNative::queryResultAvailable(GLTimerQuery* query) {
    GLuint available = 0;
    glGetQueryObjectuiv(query->gl.query, GL_QUERY_RESULT_AVAILABLE, &available);
    CHECK_GL_ERROR(utils::slog.e)
    return available != 0;
}

uint64_t TimerQueryNative::queryResult(GLTimerQuery* query) {
    GLuint64 elapsedTime = 0;
    // IOS doesn't have glGetQueryObjectui64v, we'll never end-up here on ios anyways
#ifndef IOS
    glGetQueryObjectui64v(query->gl.query, GL_QUERY_RESULT, &elapsedTime);
#endif
    CHECK_GL_ERROR(utils::slog.e)
    return elapsedTime;
}

// ------------------------------------------------------------------------------------------------

TimerQueryFence::TimerQueryFence(backend::OpenGLPlatform& platform)
        : mPlatform(platform) {
    mQueue.reserve(2);
    mThread = std::thread([this]() {
        auto& queue = mQueue;
        bool exitRequested;
        do {
            std::unique_lock<utils::Mutex> lock(mLock);
            mCondition.wait(lock, [this, &queue]() -> bool {
                return mExitRequested || !queue.empty();
            });
            exitRequested = mExitRequested;
            if (!queue.empty()) {
                Job job(queue.front());
                queue.erase(queue.begin());
                lock.unlock();
                job();
            }
        } while (!exitRequested);
    });
}

TimerQueryFence::~TimerQueryFence() {
    if (mThread.joinable()) {
        std::unique_lock<utils::Mutex> lock(mLock);
        mExitRequested = true;
        lock.unlock();
        mCondition.notify_one();
        mThread.join();
    }
}

void TimerQueryFence::enqueue(TimerQueryFence::Job&& job) {
    std::unique_lock<utils::Mutex> lock(mLock);
    mQueue.push_back(std::forward<Job>(job));
    lock.unlock();
    mCondition.notify_one();
}

void TimerQueryFence::beginTimeElapsedQuery(GLTimerQuery* query) {
    Platform::Fence* fence = mPlatform.createFence();
    query->gl.emulation.available.store(false);
    push([this, fence, query]() {
        mPlatform.waitFence(fence, FENCE_WAIT_FOR_EVER);
        query->gl.emulation.elapsed = clock::now().time_since_epoch().count();
        mPlatform.destroyFence(fence);
    });
}

void TimerQueryFence::endTimeElapsedQuery(GLTimerQuery* query) {
    Platform::Fence* fence = mPlatform.createFence();
    push([this, fence, query]() {
        mPlatform.waitFence(fence, FENCE_WAIT_FOR_EVER);
        query->gl.emulation.elapsed = clock::now().time_since_epoch().count() - query->gl.emulation.elapsed;
        query->gl.emulation.available.store(true);
        mPlatform.destroyFence(fence);
    });
}

bool TimerQueryFence::queryResultAvailable(GLTimerQuery* query) {
    return query->gl.emulation.available.load();
}

uint64_t TimerQueryFence::queryResult(GLTimerQuery* query) {
    return query->gl.emulation.elapsed;
}

// ------------------------------------------------------------------------------------------------

TimerQueryFallback::TimerQueryFallback() = default;

TimerQueryFallback::~TimerQueryFallback() = default;

void TimerQueryFallback::beginTimeElapsedQuery(TimerQueryInterface::GLTimerQuery* query) {
    // this implementation clearly doesn't work at all, but we have no h/w support
    query->gl.emulation.available.store(false, std::memory_order_relaxed);
    query->gl.emulation.elapsed = clock::now().time_since_epoch().count();
}

void TimerQueryFallback::endTimeElapsedQuery(TimerQueryInterface::GLTimerQuery* query) {
    // this implementation clearly doesn't work at all, but we have no h/w support
    query->gl.emulation.elapsed = clock::now().time_since_epoch().count() - query->gl.emulation.elapsed;
    query->gl.emulation.available.store(true, std::memory_order_relaxed);
}

bool TimerQueryFallback::queryResultAvailable(TimerQueryInterface::GLTimerQuery* query) {
    return query->gl.emulation.available.load(std::memory_order_relaxed);
}

uint64_t TimerQueryFallback::queryResult(TimerQueryInterface::GLTimerQuery* query) {
    return query->gl.emulation.elapsed;
}

} // namespace filament