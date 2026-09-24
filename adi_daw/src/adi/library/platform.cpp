// SPDX-License-Identifier: GPL-3.0-or-later
#include "volume.hpp"
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
namespace adi::library {
bool lowerWorkerPriority() noexcept {
#if defined(_WIN32)
    return SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != 0;
#elif defined(__APPLE__)
    return pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0) == 0;
#else
    return setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10) == 0;
#endif
}
}
