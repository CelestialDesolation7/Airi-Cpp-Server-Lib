#include "Poller/Poller.h"

#ifdef __linux__
#include "Poller/EpollPoller.h"
#elif defined(__APPLE__) || defined(__FreeBSD__)
#include "Poller/KqueuePoller.h"
#else
#error "OS not supported!"
#endif

Poller *Poller::newDefaultPoller(Eventloop *loop) {
#ifdef __linux__
    return new EpollPoller(loop);
#else
    return new KqueuePoller(loop);
#endif
}