#include <stdint.h>
#include <unistd.h>
#include <xen/xen.h>
#include <xen/event_channel.h> // kernel interface
#include <xen/sys/evtchn.h>    // userspace interface

#include "core/reactor.hh"
#include "core/semaphore.hh"
#include "core/future-util.hh"
#include "evtchn.hh"
#include "osv_xen.hh"

semaphore *evtchn::init_port(int port) {
    _promises.emplace(port, semaphore(0));
    return port_to_sem(port);
}

void evtchn::make_ready_port(int port) {
    auto sem = port_to_sem(port);
    sem->signal();
}

future<> evtchn::pending(int port) {
    auto sem = port_to_sem(port);
    return sem->wait();
}

class userspace_evtchn: public evtchn {
    pollable_fd _evtchn;
    void umask(int *port, unsigned count);
public:
    userspace_evtchn(unsigned otherend);
    virtual int bind() override;
    virtual void notify(int port) override;
};

userspace_evtchn::userspace_evtchn(unsigned otherend)
    : evtchn(otherend)
    , _evtchn(pollable_fd(file_desc::open("/dev/xen/evtchn", O_RDWR | O_NONBLOCK)))
{
    keep_doing([this] {
        int ports[2];
        return _evtchn.read_some(reinterpret_cast<char *>(&ports), sizeof(ports)).then([this, &ports] (size_t s)
        {
            auto count = s / sizeof(ports[0]);
            umask(ports, count);
            for (unsigned i = 0; i < count; ++i) {
                make_ready_port(ports[i]);
            }
            make_ready_future<>();
        });
    });
}

int userspace_evtchn::bind()
{
    struct ioctl_evtchn_bind_unbound_port bind = { _otherend };

    auto ret = _evtchn.get_file_desc().ioctl<struct ioctl_evtchn_bind_unbound_port>(IOCTL_EVTCHN_BIND_UNBOUND_PORT, bind);
    init_port(ret);
    return ret;
}

void userspace_evtchn::notify(int port)
{
    struct ioctl_evtchn_notify notify;
    notify.port = port;

    _evtchn.get_file_desc().ioctl<struct ioctl_evtchn_notify>(IOCTL_EVTCHN_NOTIFY, notify);
}

void userspace_evtchn::umask(int *port, unsigned count)
{
    _evtchn.get_file_desc().write(port, count * sizeof(port));
}

#ifdef HAVE_OSV
class kernel_evtchn: public evtchn {
    static void make_ready(void *arg);
    static void process_interrupts(readable_eventfd* fd, semaphore* sem);
public:
    kernel_evtchn(unsigned otherend) : evtchn(otherend) {}
    virtual int bind() override;
    virtual void notify(int port) override;
};

void kernel_evtchn::make_ready(void *arg) {
    printf("Got an interrupt!\n");
    int fd = reinterpret_cast<uintptr_t>(arg);
    uint64_t one = 1;
    ::write(fd, &one, sizeof(one));
}

int kernel_evtchn::bind() {

    unsigned int irq;

    int port;
    irq = bind_listening_port_to_irq(_otherend, &port);
    // We need to convert extra-seastar events to intra-seastar events
    // (in this case, the semaphore interface of evtchn).  The only
    // way to do that currently is via eventfd.
    semaphore* sem = init_port(port);
    auto fd = new readable_eventfd;
    auto wfd = fd->get_write_fd();
    intr_add_handler("", irq, NULL, make_ready, reinterpret_cast<void*>(uintptr_t(wfd)), 0, 0);
    unmask_evtchn(port);
    process_interrupts(fd, sem);
    return evtchn_from_irq(irq);
}

void kernel_evtchn::process_interrupts(readable_eventfd* fd, semaphore* sem) {
    fd->wait().then([fd, sem] (size_t ignore) {
        sem->signal();
        process_interrupts(fd, sem);
    });
}

void kernel_evtchn::notify(int port) {
    notify_remote_via_evtchn(port);
}
#endif

evtchn *evtchn::_instance = nullptr;
evtchn *evtchn::instance(bool userspace, unsigned otherend)
{
    if (!_instance) {
#ifdef HAVE_OSV
        if (!userspace) {
            _instance = new kernel_evtchn(otherend);
        } else
#endif
            _instance = new userspace_evtchn(otherend);
    }
    return _instance;
}
