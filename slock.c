/* See LICENSE file for license details. */
#define _XOPEN_SOURCE 500

#if HAVE_SHADOW_H
#    include <shadow.h>
#endif

enum { INIT, INPUT, FAILED, NUMCOLS };

#include "arg.h"
#include "config.h"

#include <Imlib2.h>
#include <X11/XF86keysym.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <X11/keysym.h>
#include <crypt.h>
#include <ctype.h>
#include <errno.h>
#include <grp.h>
#include <linux/oom.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

char* argv0;

struct lock {
    int screen;
    Window root, win;
    Pixmap pmap;
    Pixmap bgmap;
    unsigned long colors[NUMCOLS];
};

struct xrandr {
    int active;
    int evbase;
    int errbase;
};

static void die(const char* errstr, ...) {
    va_list ap;

    va_start(ap, errstr);
    vfprintf(stderr, errstr, ap);
    va_end(ap);
    exit(1);
}

static void dontkillme(void) {
    const char oomfile[] = "/proc/self/oom_score_adj";

    FILE* f = fopen(oomfile, "w");
    if (!f) {
        if (errno == ENOENT) return;
        die("slock: fopen %s: %s\n", oomfile, strerror(errno));
    }
    fprintf(f, "%d", OOM_SCORE_ADJ_MIN);
    if (fclose(f)) {
        if (errno == EACCES) {
            die("slock: unable to disable OOM killer. "
                "Make sure to suid or sgid slock.\n");
        } else {
            die("slock: fclose %s: %s\n", oomfile, strerror(errno));
        }
    }
}

static const char* gethash(void) {
    /* Check if the current user has a password entry */
    errno = 0;
    struct passwd* pw = getpwuid(getuid());
    if (!pw) {
        if (errno) {
            die("slock: getpwuid: %s\n", strerror(errno));
        } else {
            die("slock: cannot retrieve password entry\n");
        }
    }
    const char* hash = pw->pw_passwd;

#if HAVE_SHADOW_H
    if (!strcmp(hash, "x")) {
        struct spwd* sp;
        if (!(sp = getspnam(pw->pw_name))) {
            die("slock: getspnam: cannot retrieve shadow entry. "
                "Make sure to suid or sgid slock.\n");
        }
        hash = sp->sp_pwdp;
    }
#else
    if (!strcmp(hash, "*")) {
        die("slock: getpwuid: cannot retrieve shadow entry. "
            "Make sure to suid or sgid slock.\n");
    }
#endif /* HAVE_SHADOW_H */

    return hash;
}

static void
readpw(Display* dpy, struct xrandr* rr, struct lock** locks, int nscreens, const char* hash) {
    char passwd[256];

    unsigned int len = 0;
    int running = 1;
    int failure = 0;
    unsigned int oldc = INIT;

    XEvent ev;
    while (running && !XNextEvent(dpy, &ev)) {
        if (ev.type == KeyPress) {
            char buf[32];
            memset(&buf, 0, sizeof(buf));
            KeySym ksym;
            int num = XLookupString(&ev.xkey, buf, sizeof(buf), &ksym, 0);
            if (IsKeypadKey(ksym)) {
                if (ksym == XK_KP_Enter) {
                    ksym = XK_Return;
                } else if (ksym >= XK_KP_0 && ksym <= XK_KP_9) {
                    ksym = (ksym - XK_KP_0) + XK_0;
                }
            }
            if (IsFunctionKey(ksym) || IsKeypadKey(ksym) || IsMiscFunctionKey(ksym) ||
                IsPFKey(ksym) || IsPrivateKeypadKey(ksym)) {
                continue;
            }
            switch (ksym) {
                case XF86XK_AudioPlay:
                case XF86XK_AudioStop:
                case XF86XK_AudioPrev:
                case XF86XK_AudioNext:
                case XF86XK_AudioRaiseVolume:
                case XF86XK_AudioLowerVolume:
                case XF86XK_AudioMute:
                case XF86XK_AudioMicMute:
                case XF86XK_MonBrightnessDown:
                case XF86XK_MonBrightnessUp:
                    XSendEvent(dpy, DefaultRootWindow(dpy), True, KeyPressMask, &ev);
                    break;
                case XK_Return:
                    passwd[len] = '\0';
                    errno = 0;
                    char* inputhash = crypt(passwd, hash);
                    if (!inputhash) {
                        fprintf(stderr, "slock: crypt: %s\n", strerror(errno));
                    } else {
                        running = !!strcmp(inputhash, hash);
                    }
                    if (running) {
                        XBell(dpy, 100);
                        failure = 1;
                    }
                    memset(&passwd, 0, sizeof(passwd));
                    len = 0;
                    break;
                case XK_Escape:
                    memset(&passwd, 0, sizeof(passwd));
                    len = 0;
                    break;
                case XK_BackSpace:
                    if (len) {
                        passwd[--len] = '\0';
                    }
                    break;
                default:
                    if (num && !iscntrl((int) buf[0]) && (len + num < sizeof(passwd))) {
                        memcpy(passwd + len, buf, num);
                        len += num;
                    }
                    break;
            }
            unsigned int color = len ? INPUT : ((failure || failonclear) ? FAILED : INIT);
            if (running && oldc != color) {
                for (int screen = 0; screen < nscreens; screen++) {
                    XSetWindowBackgroundPixmap(dpy, locks[screen]->win, locks[screen]->bgmap);
                    XClearWindow(dpy, locks[screen]->win);
                }
                oldc = color;
            }
        } else if (rr->active && ev.type == rr->evbase + RRScreenChangeNotify) {
            XRRScreenChangeNotifyEvent* rre = (XRRScreenChangeNotifyEvent*) &ev;
            for (int screen = 0; screen < nscreens; screen++) {
                if (locks[screen]->win == rre->window) {
                    if (rre->rotation == RR_Rotate_90 || rre->rotation == RR_Rotate_270) {
                        XResizeWindow(dpy, locks[screen]->win, rre->height, rre->width);
                    } else {
                        XResizeWindow(dpy, locks[screen]->win, rre->width, rre->height);
                    }
                    XClearWindow(dpy, locks[screen]->win);
                    break;
                }
            }
        } else {
            for (int screen = 0; screen < nscreens; screen++) {
                XRaiseWindow(dpy, locks[screen]->win);
            }
        }
    }
}

static struct lock* lockscreen(Display* dpy, struct xrandr* rr, int screen, Imlib_Image* image) {

    struct lock* lock = malloc(sizeof(struct lock));

    if (dpy == NULL || screen < 0 || !lock) {
        return NULL;
    }

    lock->screen = screen;
    lock->root = RootWindow(dpy, lock->screen);

    lock->bgmap = XCreatePixmap(
        dpy,
        lock->root,
        DisplayWidth(dpy, lock->screen),
        DisplayHeight(dpy, lock->screen),
        DefaultDepth(dpy, lock->screen));
    imlib_context_set_image(*image);
    imlib_context_set_display(dpy);
    imlib_context_set_visual(DefaultVisual(dpy, lock->screen));
    imlib_context_set_colormap(DefaultColormap(dpy, lock->screen));
    imlib_context_set_drawable(lock->bgmap);
    imlib_render_image_on_drawable(0, 0);
    imlib_free_image();

    for (int i = 0; i < NUMCOLS; i++) {
        XColor color, dummy;
        XAllocNamedColor(dpy, DefaultColormap(dpy, lock->screen), colorname[i], &color, &dummy);
        lock->colors[i] = color.pixel;
    }

    /* init */
    XSetWindowAttributes wa;
    wa.override_redirect = 1;
    wa.background_pixel = lock->colors[INIT];
    lock->win = XCreateWindow(
        dpy,
        lock->root,
        0,
        0,
        DisplayWidth(dpy, lock->screen),
        DisplayHeight(dpy, lock->screen),
        0,
        DefaultDepth(dpy, lock->screen),
        CopyFromParent,
        DefaultVisual(dpy, lock->screen),
        CWOverrideRedirect | CWBackPixel,
        &wa);
    if (image && *image) {
        XSetWindowBackgroundPixmap(dpy, lock->win, lock->bgmap);
    }
    char curs[] = {0, 0, 0, 0, 0, 0, 0, 0};
    lock->pmap = XCreateBitmapFromData(dpy, lock->win, curs, 8, 8);
    XColor color;
    Cursor invisible = XCreatePixmapCursor(dpy, lock->pmap, lock->pmap, &color, &color, 0, 0);
    XDefineCursor(dpy, lock->win, invisible);

    /* Try to grab mouse pointer *and* keyboard for 600ms, else fail the lock */
    int ptgrab = -1;
    int kbgrab = -1;
    for (int i = 0; i < 6; i++) {
        if (ptgrab != GrabSuccess) {
            ptgrab = XGrabPointer(
                dpy,
                lock->root,
                False,
                ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                GrabModeAsync,
                GrabModeAsync,
                None,
                invisible,
                CurrentTime);
        }
        if (kbgrab != GrabSuccess) {
            kbgrab =
                XGrabKeyboard(dpy, lock->root, True, GrabModeAsync, GrabModeAsync, CurrentTime);
        }

        /* input is grabbed: we can lock the screen */
        if (ptgrab == GrabSuccess && kbgrab == GrabSuccess) {
            XMapRaised(dpy, lock->win);
            if (rr->active) {
                XRRSelectInput(dpy, lock->win, RRScreenChangeNotifyMask);
            }

            XSelectInput(dpy, lock->root, SubstructureNotifyMask);
            return lock;
        }

        /* retry on AlreadyGrabbed but fail on other errors */
        if ((ptgrab != AlreadyGrabbed && ptgrab != GrabSuccess) ||
            (kbgrab != AlreadyGrabbed && kbgrab != GrabSuccess)) {
            break;
        }

        usleep(100000);
    }

    /* we couldn't grab all input: fail out */
    if (ptgrab != GrabSuccess) {
        fprintf(stderr, "slock: unable to grab mouse pointer for screen %d\n", screen);
    }
    if (kbgrab != GrabSuccess) {
        fprintf(stderr, "slock: unable to grab keyboard for screen %d\n", screen);
    }
    return NULL;
}

static void usage(void) {
    die("usage: slock [-v] [cmd [arg ...]]\n");
}

struct Pixel {
    union {
        struct {
            unsigned char a;
            unsigned char red;
            unsigned char green;
            unsigned char blue;
        };
        DATA32 data;
    };
};

#define MIN(A, B) (A < B ? A : B)

void compute_pixel(
    Screen* scr, DATA32* data, unsigned int x, unsigned int y, unsigned int pixel_size) {
    unsigned int red = 0;
    unsigned int green = 0;
    unsigned int blue = 0;
    unsigned int alpha = 0;

    unsigned int width = scr->width;
    unsigned int height = scr->height;

    unsigned int height_rect = MIN(pixel_size, height - y);
    unsigned int width_rect = MIN(pixel_size, width - x);

    struct Pixel p;
    for (unsigned int j = 0; j < height_rect; j++) {
        for (unsigned int i = 0; i < width_rect; i++) {
            p.data = data[width * (y + j) + (x + i)];
            alpha += p.a;
            red += p.red;
            green += p.green;
            blue += p.blue;
        }
    }
    unsigned int rect_area = height_rect * width_rect;

    p.red = red / rect_area;
    p.green = green / rect_area;
    p.blue = blue / rect_area;
    p.a = alpha / rect_area;

    for (unsigned int j = 0; j < height_rect; j++) {
        for (unsigned int i = 0; i < width_rect; i++) {
            data[width * (y + j) + (x + i)] = p.data;
        }
    }
}

int main(int argc, char** argv) {
    int s, nlocks;

    ARGBEGIN {
        case 'v':
            fprintf(stderr, "slock-" VERSION "\n");
            return 0;
        default:
            usage();
    }
    ARGEND

    /* validate drop-user and -group */
    errno = 0;
    struct passwd* pwd = getpwnam(user);
    if (!pwd) {
        die("slock: getpwnam %s: %s\n", user, errno ? strerror(errno) : "user entry not found");
    }
    uid_t duid = pwd->pw_uid;
    errno = 0;
    struct group* grp = getgrnam(group);
    if (!grp) {
        die("slock: getgrnam %s: %s\n", group, errno ? strerror(errno) : "group entry not found");
    }
    gid_t dgid = grp->gr_gid;

    dontkillme();

    const char* hash = gethash();
    errno = 0;
    if (!crypt("", hash)) {
        die("slock: crypt: %s\n", strerror(errno));
    }

    Display* dpy = XOpenDisplay(NULL);
    if (!dpy) {
        die("slock: cannot open display\n");
    }

    /* drop privileges */
    if (setgroups(0, NULL) < 0) {
        die("slock: setgroups: %s\n", strerror(errno));
    }
    if (setgid(dgid) < 0) {
        die("slock: setgid: %s\n", strerror(errno));
    }
    if (setuid(duid) < 0) {
        die("slock: setuid: %s\n", strerror(errno));
    }

    /*Create screenshot Image*/
    Screen* scr = ScreenOfDisplay(dpy, DefaultScreen(dpy));
    Imlib_Image image = imlib_create_image(scr->width, scr->height);
    imlib_context_set_image(image);
    imlib_context_set_display(dpy);
    imlib_context_set_visual(DefaultVisual(dpy, 0));
    imlib_context_set_drawable(RootWindow(dpy, XScreenNumberOfScreen(scr)));
    imlib_copy_drawable_to_image(0, 0, 0, scr->width, scr->height, 0, 0, 1);

    if (!image) {
        die("could not take screenshot");
    }

    /*Pixelation*/
    unsigned int width = scr->width;
    unsigned int height = scr->height;

    DATA32* data = imlib_image_get_data();

    for (unsigned int y = 0; y < height; y = MIN(y + pixel_size, height)) {
        for (unsigned int x = 0; x < width; x = MIN(x + pixel_size, width)) {
            compute_pixel(scr, data, x, y, pixel_size);
        }
    }

    imlib_image_put_back_data(data);

    /* check for Xrandr support */
    struct xrandr rr;
    rr.active = XRRQueryExtension(dpy, &rr.evbase, &rr.errbase);

    /* get number of screens in display "dpy" and blank them */
    int nscreens = ScreenCount(dpy);
    struct lock** locks = calloc(nscreens, sizeof(struct lock*));
    if (!locks) {
        die("slock: out of memory\n");
    }
    for (nlocks = 0, s = 0; s < nscreens; s++) {
        if ((locks[s] = lockscreen(dpy, &rr, s, &image)) != NULL) {
            nlocks++;
        } else {
            break;
        }
    }
    XSync(dpy, 0);

    /* did we manage to lock everything? */
    if (nlocks != nscreens) {
        return 1;
    }

    /* everything is now blank. Wait for the correct password */
    readpw(dpy, &rr, locks, nscreens, hash);

    return 0;
}
