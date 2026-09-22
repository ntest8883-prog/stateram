
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

static void show_errno(const char *what) {
    printf("%-34s FAIL errno=%d (%s)\n", what, errno, strerror(errno));
}

static void api_probe(int uffd, const char *label) {
    struct uffdio_api ua;
    memset(&ua, 0, sizeof(ua));
    ua.api = UFFD_API;
    ua.features = 0;

    if (ioctl(uffd, UFFDIO_API, &ua) == -1) {
        show_errno(label);
        return;
    }

    printf("%-34s OK\n", label);
    printf("  API:      0x%llx\n", (unsigned long long)ua.api);
    printf("  features: 0x%llx\n", (unsigned long long)ua.features);
    printf("  ioctls:   0x%llx\n", (unsigned long long)ua.ioctls);
}

int main(void) {
    struct utsname u;
    if (uname(&u) == 0)
        printf("Kernel: %s %s %s\n", u.sysname, u.release, u.machine);

    FILE *f = fopen("/proc/sys/vm/unprivileged_userfaultfd", "r");
    if (f) {
        int v=-1;
        if (fscanf(f, "%d", &v) == 1)
            printf("vm.unprivileged_userfaultfd: %d\n", v);
        fclose(f);
    } else {
        printf("vm.unprivileged_userfaultfd: unavailable\n");
    }

    printf("/dev/userfaultfd: %s\n",
           access("/dev/userfaultfd", F_OK) == 0 ? "present" : "absent");

    printf("\nPath A: userfaultfd(2) + UFFD_USER_MODE_ONLY\n");
    errno=0;
    int uffd = (int)syscall(SYS_userfaultfd,
                            O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    if (uffd < 0) {
        show_errno("userfaultfd syscall");
    } else {
        printf("%-34s OK fd=%d\n","userfaultfd syscall",uffd);
        api_probe(uffd,"UFFDIO_API via syscall fd");
        close(uffd);
    }

#ifdef USERFAULTFD_IOC_NEW
    printf("\nPath B: /dev/userfaultfd + USERFAULTFD_IOC_NEW\n");
    errno=0;
    int dev = open("/dev/userfaultfd", O_RDWR | O_CLOEXEC);
    if (dev < 0) {
        show_errno("open /dev/userfaultfd");
    } else {
        printf("%-34s OK fd=%d\n","open /dev/userfaultfd",dev);
        errno=0;
        int uffd2 = ioctl(dev, USERFAULTFD_IOC_NEW,
                          O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
        if (uffd2 < 0) {
            show_errno("USERFAULTFD_IOC_NEW");
        } else {
            printf("%-34s OK fd=%d\n","USERFAULTFD_IOC_NEW",uffd2);
            api_probe(uffd2,"UFFDIO_API via device fd");
            close(uffd2);
        }
        close(dev);
    }
#else
    printf("\nPath B unavailable: headers lack USERFAULTFD_IOC_NEW\n");
#endif

    printf("\nInterpretation:\n");
    printf("  ENOSYS on syscall path: kernel/syscall unavailable or blocked by sandbox/seccomp.\n");
    printf("  EPERM on syscall path: permission/policy restriction.\n");
    printf("  ENOENT on device path: /dev/userfaultfd is not exposed.\n");
    printf("  EACCES/EPERM on device path: device exists but permissions/policy block it.\n");
    printf("  UFFDIO_API OK: StateRAM-8.1 has the core mechanism it needs.\n");
    return 0;
}
