// libhwp-fastpatch.so: fast, partial repaints and real scrolling for Hancom's
// libhsp.so (Hwp's Win32 emulation layer).
//
// patch-libhsp.py routes four libhsp call sites to hwp_fastpatch_entry()
// (through libhsp's renamed QImage::sizeInBytes() PLT slot); the call site is
// recognised by the return address:
//   * the window-image fingerprint (DC create / DC release), see below;
//   * ATL::CWindow::ScrollWindow -> ScrollWindow(), see "Scrolling" below;
//   * ATL::CWindow::UpdateWindow -> UpdateWindow().
//
// Repaints
// --------
// libhsp.so (Hancom's Win32 emulation layer) keeps a back-buffer QImage per
// window. When a DC is created it stores a fingerprint of the whole image
// (an MD5 in 11.20.0.1520); when the DC is released it fingerprints the image
// again and, if the two differ, calls QWidget::update(): a repaint of the
// *whole* window (blend + clear + X11 upload of ~15 MB on a 2540x1500
// window) for every keystroke. "No change -> no update" is also what stops
// libhsp's repaint -> WM_PAINT -> repaint loop, so the check has to stay.
//
// libhsp's fingerprint function tail-calls hwp_fastpatch_entry() (rdi =
// QByteArray *result, rsi = window holder with the QWidget * at +0x10 and
// the QImage at +0x18). Instead of a fingerprint it
//   * keeps a copy of each window image plus a 64-bit XXH3 hash per row,
//   * on DC release hashes the rows (in parallel), compares them with the
//     stored hashes, finds the changed spans of changed rows, updates the
//     copy, and calls QWidget::update(region) for the changed rectangles only,
//   * always returns the same token, so libhsp's own full update() never runs.
//
// Scrolling
// ---------
// Hwp's view scrolls like a Win32 program: ScrollWindow(dx, dy) to move the
// pixels, then UpdateWindow() to paint the exposed strip. In libhsp,
// ScrollWindow() does nothing and UpdateWindow() is QWidget::repaint(), so
// every scroll step re-renders the whole view (3 steps per mouse-wheel notch).
// For the document view (the only user of CWindow::ScrollWindow) this file
// implements ScrollWindow properly: move the back-buffer pixels (and the
// snapshot above), move the on-screen pixels with QWidget::scroll(), which
// leaves only the exposed strip to repaint, and let UpdateWindow() on such a
// window paint only the pending (invalid) region, like Win32 does.
//
// Environment:
//   HWP_REPAINT=hash          whole-window updates with an XXH3 fingerprint
//                             (libhsp's original logic, only faster)
//   HWP_FASTPATCH_THREADS=N   threads for row hashing (default: min(4, CPUs))
//   HWP_FASTPATCH_DUMP=DIR    debug: dump the back buffer (raw BGRA) to DIR
//                             on every DC release
//   HWP_FASTPATCH_DEBUG=1     debug: log scroll, wheel and diff decisions
//   HWP_FASTSCROLL=0          keep libhsp's ScrollWindow/UpdateWindow and do
//                             not coalesce wheel events
//
// Wheel events
// ------------
// Hwp handles every wheel notch synchronously (~100+ ms of UI work per
// scroll step), so notches that arrive meanwhile queue up and the view keeps
// scrolling long after the wheel stopped. An application event filter merges
// wheel events for the document view that are already queued into one event
// with the summed delta (Hwp scrolls notches x lines in one step for it).
#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QPointer>
#include <QtGui/QWheelEvent>
#include <QtGui/QImage>
#include <QtGui/QRegion>
#include <QtWidgets/QAbstractScrollArea>
#include <QtWidgets/QWidget>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace {

// Return addresses of the two fingerprint calls inside libhsp.so 11.20.0.1520
// (checked by patch-libhsp.py).
constexpr uintptr_t kStartCall = 0x16b06e;  // after CreateDCW
constexpr uintptr_t kEndCall = 0x16a6ac;    // DC release: compare + update()
constexpr uintptr_t kScrollCall = 0x0e99cc; // ATL::CWindow::ScrollWindow
constexpr uintptr_t kUpdateCall = 0x0e9c1c; // ATL::CWindow::UpdateWindow

struct Snapshot {
    const uchar *bits = nullptr;
    int width = 0, height = 0;
    qsizetype bytesPerLine = 0;
    QImage::Format format = QImage::Format_Invalid;
    std::vector<uchar> data;            // copy of the image
    std::vector<XXH64_hash_t> rowHash;  // XXH3 of each row of `data`
    bool stale = false; // data/rowHash out of date (after a large change);
                        // only the sample rows (y % kSample == 0) are current
};

// Large changes (scrolling, page changes) are detected from every kSample-th
// row: if more than kLargeChange of them changed, the whole image is
// repainted without hashing every row or updating the copy.
constexpr int kSample = 8;
constexpr double kLargeChange = 0.25;

std::recursive_mutex lock; // recursive: flushPaint() runs the DC-release diff
std::unordered_map<const void *, Snapshot> snapshots;
std::vector<XXH64_hash_t> rowHashNow;
uintptr_t hspBase;
std::unordered_map<const void *, const void *> scrolledWindows; // hwnd -> holder

const char *env(const char *name)
{
    const char *v = getenv(name);
    return v && *v ? v : nullptr;
}

bool fastScroll()
{
    static const bool on = [] {
        const char *e = env("HWP_FASTSCROLL");
        return !(e && !strcmp(e, "0"));
    }();
    return on;
}

bool hashMode()
{
    static const bool on = [] {
        const char *e = env("HWP_REPAINT");
        return e && !strcmp(e, "hash");
    }();
    return on;
}

int threads()
{
    static const int n = [] {
        if (const char *e = env("HWP_FASTPATCH_THREADS"))
            return qBound(1, atoi(e), 16);
        return qBound(1, int(std::thread::hardware_concurrency()), 4);
    }();
    return n;
}

uintptr_t callerOffset(void *ra)
{
    if (!hspBase) {
        Dl_info info;
        if (!dladdr(ra, &info) || !info.dli_fname || !strstr(info.dli_fname, "libhsp.so"))
            return 0;
        hspBase = reinterpret_cast<uintptr_t>(info.dli_fbase);
    }
    return reinterpret_cast<uintptr_t>(ra) - hspBase;
}

QByteArray xxh3(const QImage *img)
{
    const uchar *bits = img->bits();
    const XXH128_hash_t h = XXH3_128bits(bits, bits ? size_t(img->sizeInBytes()) : 0);
    return QByteArray(reinterpret_cast<const char *>(&h), sizeof h);
}

// Hash rows [0, h) into out[]: all rows, only sample rows (y % kSample == 0,
// `which` = 1) or only the other rows (`which` = 2).
void hashRows(const uchar *bits, qsizetype bpl, qsizetype rowBytes, int h, XXH64_hash_t *out,
              int which = 0)
{
    auto run = [=](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const bool sample = y % kSample == 0;
            if (which == 0 || (which == 1) == sample)
                out[y] = XXH3_64bits(bits + y * bpl, size_t(rowBytes));
        }
    };
    const int parts = which == 1 ? 1 : qMin(threads(), qMax(1, h / 64));
    const int step = (h + parts - 1) / parts;
    std::vector<std::future<void>> jobs;
    for (int p = 1; p < parts; ++p)
        jobs.push_back(std::async(std::launch::async, run, p * step, qMin(h, (p + 1) * step)));
    run(0, qMin(h, step));
    for (auto &j : jobs)
        j.get();
}

// Leftmost / rightmost differing byte of two rows: [lo, hi).
void rowSpan(const uchar *a, const uchar *b, qsizetype len, qsizetype *lo, qsizetype *hi)
{
    qsizetype l = 0, h = len;
    while (l + 8 <= len && !memcmp(a + l, b + l, 8)) l += 8;
    while (l < len && a[l] == b[l]) ++l;
    while (h - 8 >= l && !memcmp(a + h - 8, b + h - 8, 8)) h -= 8;
    while (h > l && a[h - 1] == b[h - 1]) --h;
    *lo = l;
    *hi = h;
}

// Diff img against its snapshot, bring the snapshot up to date, and return
// the changed region in image pixels.
QRegion diffAndSync(const void *holder, const QImage *img)
{
    const uchar *bits = img->constBits();
    const int w = img->width(), h = img->height(), bpp = img->depth() / 8;
    if (!bits || w <= 0 || h <= 0 || bpp <= 0)
        return {};
    const qsizetype bpl = img->bytesPerLine(), rowBytes = qsizetype(w) * bpp;
    if (snapshots.size() > 32 && !snapshots.count(holder))
        snapshots.clear(); // windows come and go; keep this small
    Snapshot &s = snapshots[holder];
    if (s.bits != bits || s.width != w || s.height != h || s.bytesPerLine != bpl
        || s.format != img->format()) { // new or resized image: copy, repaint all
        s.bits = bits; s.width = w; s.height = h; s.bytesPerLine = bpl;
        s.format = img->format();
        s.data.assign(bits, bits + bpl * h);
        s.rowHash.resize(h);
        hashRows(bits, bpl, rowBytes, h, s.rowHash.data());
        s.stale = false;
        return QRegion(0, 0, w, h);
    }
    // 1. Sample rows: how much changed?
    rowHashNow.resize(h);
    hashRows(bits, bpl, rowBytes, h, rowHashNow.data(), 1);
    int samples = 0, changed = 0;
    for (int y = 0; y < h; y += kSample, ++samples)
        changed += rowHashNow[y] != s.rowHash[y];
    const bool large = changed > kLargeChange * samples;
    if (large || s.stale) {
        if (large) { // repaint everything; skip the full diff and copy
            for (int y = 0; y < h; y += kSample)
                s.rowHash[y] = rowHashNow[y];
            s.stale = true;
        } else { // changes are small again: resynchronise once
            s.data.assign(bits, bits + bpl * h);
            hashRows(bits, bpl, rowBytes, h, s.rowHash.data());
            s.stale = false;
        }
        return QRegion(0, 0, w, h);
    }
    // 2. Small change: hash the remaining rows and diff.
    hashRows(bits, bpl, rowBytes, h, rowHashNow.data(), 2);

    QRegion region;
    int bandTop = -1;
    qsizetype bandLo = 0, bandHi = 0;
    auto flush = [&](int bottom) {
        if (bandTop < 0)
            return;
        const int x0 = int(bandLo / bpp), x1 = int((bandHi + bpp - 1) / bpp);
        region += QRect(x0, bandTop, x1 - x0, bottom - bandTop);
        bandTop = -1;
    };
    for (int y = 0; y < h; ++y) {
        if (rowHashNow[y] == s.rowHash[y]) {
            flush(y);
            continue;
        }
        s.rowHash[y] = rowHashNow[y];
        const uchar *cur = bits + y * bpl;
        uchar *old = s.data.data() + y * bpl;
        qsizetype lo, hi;
        rowSpan(cur, old, rowBytes, &lo, &hi);
        if (lo >= hi) { // same bytes after all
            flush(y);
            continue;
        }
        memcpy(old + lo, cur + lo, hi - lo);
        if (bandTop < 0) {
            bandTop = y; bandLo = lo; bandHi = hi;
        } else {
            bandLo = qMin(bandLo, lo); bandHi = qMax(bandHi, hi);
        }
    }
    flush(h);
    return region;
}

// ---- Scrolling --------------------------------------------------------------

using ScrollWindowFn = int (*)(void *, int, int, const void *, const void *);
using UpdateWindowFn = int (*)(void *);

template <typename F> F hspSymbol(const char *name)
{
    static void *hsp = dlopen("libhsp.so", RTLD_LAZY | RTLD_NOLOAD);
    return reinterpret_cast<F>(hsp ? dlsym(hsp, name) : nullptr);
}

int realScrollWindow(void *hwnd, int dx, int dy, const void *rect, const void *clip)
{
    static auto fn = hspSymbol<ScrollWindowFn>("ScrollWindow");
    return fn ? fn(hwnd, dx, dy, rect, clip) : 0;
}

int realUpdateWindow(void *hwnd)
{
    static auto fn = hspSymbol<UpdateWindowFn>("UpdateWindow");
    return fn ? fn(hwnd) : 0;
}

const char *holderOf(void *hwnd)
{
    return hwnd ? *reinterpret_cast<const char *const *>(static_cast<const char *>(hwnd) + 0xd0) : nullptr;
}

// Paint the pending (invalid) region of the widget's window now.
void flushPaint(QWidget *w)
{
    QCoreApplication::sendPostedEvents(w->window(), QEvent::UpdateRequest);
}

// Move rows/columns of an image buffer by (dx, dy); the exposed area keeps
// its old content until it is repainted.
void shiftPixels(uchar *bits, qsizetype bpl, int w, int h, int bpp, int dx, int dy)
{
    if (dy < 0)
        memmove(bits, bits + qsizetype(-dy) * bpl, qsizetype(h + dy) * bpl);
    else if (dy > 0)
        memmove(bits + qsizetype(dy) * bpl, bits, qsizetype(h - dy) * bpl);
    if (dx) {
        const qsizetype n = qsizetype(w - qAbs(dx)) * bpp;
        for (int y = 0; y < h; ++y) {
            uchar *row = bits + y * bpl;
            if (dx < 0)
                memmove(row, row + qsizetype(-dx) * bpp, n);
            else
                memmove(row + qsizetype(dx) * bpp, row, n);
        }
    }
}

bool debug()
{
    static const bool on = env("HWP_FASTPATCH_DEBUG");
    return on;
}

int scrollWindow(void *hwnd, int dx, int dy, const void *rect, const void *clip)
{
    const char *holder = holderOf(hwnd);
    QWidget *w = holder ? *reinterpret_cast<QWidget *const *>(holder + 0x10) : nullptr;
    QImage *img = holder ? reinterpret_cast<QImage *>(const_cast<char *>(holder) + 0x18) : nullptr;
    if (!fastScroll() || hashMode() || rect || clip || !w || !img || img->isNull()
        || img->depth() != 32 || w->width() != img->width() || w->height() != img->height()
        || qobject_cast<QAbstractScrollArea *>(w) || (!dx && !dy)
        || qAbs(dx) >= img->width() || qAbs(dy) >= img->height()) {
        if (debug())
            fprintf(stderr, "fastpatch: ScrollWindow(%d,%d) fallback: rect=%p clip=%p w=%p img=%dx%d widget=%dx%d scrollarea=%d\n",
                    dx, dy, rect, clip, (void *)w, img ? img->width() : -1, img ? img->height() : -1,
                    w ? w->width() : -1, w ? w->height() : -1, w ? qobject_cast<QAbstractScrollArea *>(w) != nullptr : -1);
        scrolledWindows.erase(hwnd);
        return realScrollWindow(hwnd, dx, dy, rect, clip); // + full repaint later
    }
    std::lock_guard<std::recursive_mutex> guard(lock);
    flushPaint(w); // paint what is pending before the pixels move

    // the back buffer (QImage::bits() would detach; libhsp draws into these bytes)
    uchar *bits = const_cast<uchar *>(img->constBits());
    const int iw = img->width(), ih = img->height();
    const qsizetype bpl = img->bytesPerLine();
    shiftPixels(bits, bpl, iw, ih, 4, dx, dy);

    // keep the diff snapshot in step, so only the new strip counts as changed
    auto it = snapshots.find(holder);
    if (it != snapshots.end()) {
        Snapshot &s = it->second;
        if (s.bits == bits && s.width == iw && s.height == ih && s.bytesPerLine == bpl && !s.stale) {
            shiftPixels(s.data.data(), bpl, iw, ih, 4, dx, dy);
            if (dx) {
                s.stale = true; // row hashes no longer match: resynchronise later
            } else if (dy < 0) {
                std::move(s.rowHash.begin() - dy, s.rowHash.end(), s.rowHash.begin());
                for (int y = ih + dy; y < ih; ++y) s.rowHash[y] = ~s.rowHash[y];
            } else {
                std::move_backward(s.rowHash.begin(), s.rowHash.end() - dy, s.rowHash.end());
                for (int y = 0; y < dy; ++y) s.rowHash[y] = ~s.rowHash[y];
            }
        }
    }

    // move the on-screen pixels; Qt then repaints only the exposed strip
    // (accelerated scrolling needs an opaque widget; the back buffer is opaque)
    if (!w->testAttribute(Qt::WA_OpaquePaintEvent))
        w->setAttribute(Qt::WA_OpaquePaintEvent);
    w->scroll(dx, dy);
    if (debug())
        fprintf(stderr, "fastpatch: ScrollWindow(%d,%d) on %s %dx%d\n", dx, dy,
                w->metaObject()->className(), w->width(), w->height());
    scrolledWindows[hwnd] = holder;
    return 1;
}

int updateWindow(void *hwnd)
{
    auto it = scrolledWindows.find(hwnd);
    const char *holder = holderOf(hwnd);
    if (it != scrolledWindows.end() && it->second == holder && holder) {
        if (QWidget *w = *reinterpret_cast<QWidget *const *>(holder + 0x10)) {
            flushPaint(w); // Win32 semantics: paint only the invalid region
            return 1;
        }
    }
    return realUpdateWindow(hwnd); // libhsp: QWidget::repaint() of everything
}

// ---- Wheel event coalescing -------------------------------------------------

class WheelCoalescer : public QObject
{
public:
    bool eventFilter(QObject *obj, QEvent *ev) override
    {
        if (ev->type() != QEvent::Wheel || injecting || !obj->isWidgetType()
            || strcmp(obj->metaObject()->className(), "HspNativeWindow") != 0)
            return false;
        auto *we = static_cast<QWheelEvent *>(ev);
        if (pending && (target != obj || we->modifiers() != mods || we->buttons() != buttons))
            deliver();
        if (!pending) {
            target = obj; mods = we->modifiers(); buttons = we->buttons();
            angle = QPoint(); pixel = QPoint();
            QCoreApplication::postEvent(this, new QEvent(QEvent::User)); // after this batch
        }
        pending = true;
        angle += we->angleDelta(); pixel += we->pixelDelta();
        pos = we->posF(); globalPos = we->globalPosF();
        phase = we->phase(); source = we->source(); inverted = we->inverted();
        return true; // delivered (merged) by deliver()
    }

    bool event(QEvent *ev) override
    {
        if (ev->type() != QEvent::User)
            return QObject::event(ev);
        if (pending)
            deliver();
        return true;
    }

private:
    void deliver()
    {
        pending = false;
        if (!target)
            return;
        const bool vertical = qAbs(angle.y()) >= qAbs(angle.x());
        QWheelEvent merged(pos, globalPos, pixel, angle, vertical ? angle.y() : angle.x(),
                           vertical ? Qt::Vertical : Qt::Horizontal, buttons, mods, phase,
                           source, inverted);
        if (debug())
            fprintf(stderr, "fastpatch: wheel delta (%d,%d) delivered\n", angle.x(), angle.y());
        // QCoreApplication::sendEvent() is inline in Qt 5.11 (the ibus variant
        // runs on it) and exported in 5.15; both call notifyInternal2().
        using NotifyFn = bool (*)(QObject *, QEvent *);
        static auto notify = reinterpret_cast<NotifyFn>(
            dlsym(RTLD_DEFAULT, "_ZN16QCoreApplication15notifyInternal2EP7QObjectP6QEvent"));
        if (!notify)
            return;
        injecting = true;
        notify(target, &merged);
        injecting = false;
    }

    QPointer<QObject> target;
    bool pending = false, injecting = false, inverted = false;
    QPoint angle, pixel;
    QPointF pos, globalPos;
    Qt::KeyboardModifiers mods;
    Qt::MouseButtons buttons;
    Qt::ScrollPhase phase = Qt::NoScrollPhase;
    Qt::MouseEventSource source = Qt::MouseEventNotSynthesized;
};

void installWheelCoalescer()
{
    static bool done;
    if (done || !fastScroll() || hashMode() || !QCoreApplication::instance())
        return;
    done = true;
    QCoreApplication::instance()->installEventFilter(new WheelCoalescer);
}

void dumpImage(const void *holder, const QImage *img, const char *dir)
{
    char path[512], tmp[520];
    snprintf(path, sizeof path, "%s/%dx%d-%p.raw", dir, img->width(), img->height(), holder);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (FILE *f = fopen(tmp, "wb")) { // write + rename: readers never see a partial file
        for (int y = 0; y < img->height(); ++y)
            fwrite(img->constScanLine(y), size_t(img->depth() / 8), size_t(img->width()), f);
        fclose(f);
        rename(tmp, path);
    }
}

} // namespace

static QByteArray fingerprint(const char *holder, uintptr_t at)
{
    static const QByteArray token("hwp-fastpatch");
    const QImage *img = reinterpret_cast<const QImage *>(holder + 0x18);
    if (hashMode())
        return xxh3(img); // libhsp's own logic: compare fingerprints, full update()
    if (at == kStartCall)
        return token;

    std::lock_guard<std::recursive_mutex> guard(lock);
    QWidget *widget = *reinterpret_cast<QWidget *const *>(holder + 0x10);
    const QRegion dirty = diffAndSync(holder, img);
    if (debug()) {
        qint64 area = 0;
        for (const QRect &r : dirty) area += qint64(r.width()) * r.height();
        const QRect b = dirty.boundingRect();
        fprintf(stderr, "fastpatch: diff %d rects, %lld px, bbox (%d,%d %dx%d)\n", dirty.rectCount(), (long long)area, b.x(), b.y(), b.width(), b.height());
    }
    if (const char *dir = env("HWP_FASTPATCH_DUMP"))
        dumpImage(holder, img, dir);
    if (dirty.isEmpty() || !widget)
        return token;
    if (widget->width() == img->width() && widget->height() == img->height()) {
        widget->update(dirty);
    } else { // scaled (e.g. QT_SCALE_FACTOR): map to widget coordinates, round outwards
        const qreal sx = qreal(widget->width()) / img->width();
        const qreal sy = qreal(widget->height()) / img->height();
        QRegion mapped;
        for (const QRect &r : dirty)
            mapped += QRectF(r.x() * sx, r.y() * sy, r.width() * sx, r.height() * sy)
                          .toAlignedRect()
                          .adjusted(-1, -1, 1, 1);
        widget->update(mapped);
    }
    return token;
}

// Entry point for all patched libhsp call sites (integer/pointer arguments in
// rdi, rsi, rdx, rcx, r8). The caller is identified by the return address.
extern "C" __attribute__((visibility("default"))) uintptr_t
hwp_fastpatch_entry(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e)
{
    const uintptr_t at = callerOffset(__builtin_return_address(0));
    installWheelCoalescer(); // first call happens once the application runs
    switch (at) {
    case kStartCall:
    case kEndCall: // a = QByteArray *result (sret), b = window holder
        new (reinterpret_cast<void *>(a)) QByteArray(fingerprint(reinterpret_cast<const char *>(b), at));
        return a;
    case kScrollCall: // ScrollWindow(hwnd, dx, dy, rect, clip)
        return uintptr_t(scrollWindow(reinterpret_cast<void *>(a), int(b), int(c),
                                      reinterpret_cast<const void *>(d), reinterpret_cast<const void *>(e)));
    case kUpdateCall: // UpdateWindow(hwnd)
        return uintptr_t(updateWindow(reinterpret_cast<void *>(a)));
    default:
        fprintf(stderr, "libhwp-fastpatch: unexpected caller (libhsp+%#lx)\n", (unsigned long)at);
        abort();
    }
}
