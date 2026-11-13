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
//
// High-DPI UI fixes
// -----------------
// Hwp sizes its UI text by the screen DPI (Xft.dpi) but draws the formatting
// toolbar's icons at a fixed 16 px. On high-DPI screens the same event filter
//   * sets the toolbar icon size to 16 x round(DPI / 96) (32 at 144-191 DPI;
//     HWP_TOOLBAR_ICON_SIZE=N overrides, 0 or 16 = off). patch-ui.py gives the
//     toolbar Hancom's own 32 px icons where they exist; any other icon is
//     enlarged with Scale2x (pixel-art scaling: sharp, no blur);
//   * widens fixed-width toolbar boxes that are too narrow for their text
//     (the font size box drew "pt" over the number);
//   * re-sizes the menu row of the recent-file list once the list knows its
//     height (it reported 20 px when the menu was laid out, cutting off the
//     file names).
// HWP_UIFIX=0 turns these off.
#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QIcon>
#include <QtGui/QScreen>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QLayout>
#include <QtWidgets/QMenu>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QWidgetAction>
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

bool notifyObject(QObject *receiver, QEvent *ev)
{
    // QCoreApplication::sendEvent() is inline in Qt 5.11 (the ibus variant
    // runs on it) and exported in 5.15; both call notifyInternal2().
    using NotifyFn = bool (*)(QObject *, QEvent *);
    static auto notify = reinterpret_cast<NotifyFn>(
        dlsym(RTLD_DEFAULT, "_ZN16QCoreApplication15notifyInternal2EP7QObjectP6QEvent"));
    return notify && notify(receiver, ev);
}

// ---- High-DPI UI fixes ------------------------------------------------------

bool uiFixes()
{
    static const bool on = [] {
        const char *e = env("HWP_UIFIX");
        return !(e && !strcmp(e, "0"));
    }();
    return on;
}

int toolbarIconSize()
{
    static int size = -1;
    if (size < 0) {
        if (const char *e = env("HWP_TOOLBAR_ICON_SIZE")) {
            size = qBound(0, atoi(e), 128);
        } else {
            const QScreen *screen = QGuiApplication::primaryScreen();
            const qreal dpi = screen ? screen->logicalDotsPerInchY() : 96;
            size = 16 * qMax(1, qRound(dpi / 96));
        }
        if (size <= 16)
            size = 0;
    }
    return size;
}

bool isClass(const QObject *o, const char *name)
{
    return o && !strcmp(o->metaObject()->className(), name);
}

// Scale2x / EPX: doubles an image keeping hard edges sharp.
QImage scale2x(const QImage &src)
{
    const QImage in = src.convertToFormat(QImage::Format_ARGB32);
    const int w = in.width(), h = in.height();
    QImage out(w * 2, h * 2, QImage::Format_ARGB32);
    auto px = [&](int x, int y) {
        return reinterpret_cast<const QRgb *>(in.constScanLine(qBound(0, y, h - 1)))[qBound(0, x, w - 1)];
    };
    for (int y = 0; y < h; ++y) {
        QRgb *o0 = reinterpret_cast<QRgb *>(out.scanLine(2 * y));
        QRgb *o1 = reinterpret_cast<QRgb *>(out.scanLine(2 * y + 1));
        for (int x = 0; x < w; ++x) {
            const QRgb e = px(x, y), b = px(x, y - 1), d = px(x - 1, y), f = px(x + 1, y), hh = px(x, y + 1);
            o0[2 * x] = (d == b && b != f && d != hh) ? d : e;
            o0[2 * x + 1] = (b == f && b != d && f != hh) ? f : e;
            o1[2 * x] = (d == hh && d != b && hh != f) ? d : e;
            o1[2 * x + 1] = (hh == f && d != hh && b != f) ? f : e;
        }
    }
    return out;
}

QIcon upscaledIcon(const QIcon &icon, int size)
{
    int largest = 0;
    for (const QSize &s : icon.availableSizes())
        largest = qMax(largest, s.width());
    if (largest <= 0)
        return icon;
    QIcon out;
    for (QIcon::Mode mode : {QIcon::Normal, QIcon::Disabled, QIcon::Active, QIcon::Selected})
        for (QIcon::State state : {QIcon::Off, QIcon::On}) {
            QImage img = icon.pixmap(QSize(largest, largest), mode, state).toImage();
            if (img.isNull())
                continue;
            while (img.width() * 2 <= size)
                img = scale2x(img);
            if (img.width() != size)
                img = img.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            out.addPixmap(QPixmap::fromImage(img), mode, state);
        }
    return out;
}

void fixToolbar(QWidget *toolbar)
{
    const int size = toolbarIconSize();
    if (!size)
        return;
    const QSize iconSize(size, size);
    for (QToolButton *b : toolbar->findChildren<QToolButton *>()) {
        if (b->objectName() == QLatin1String("PanelScrollButtonStyle") || b->icon().isNull())
            continue;
        if (b->iconSize() != iconSize)
            b->setIconSize(iconSize);
        const qint64 key = b->property("_hwpfp_icon").toLongLong();
        if (b->icon().cacheKey() == key)
            continue;
        int largest = 0;
        for (const QSize &s : b->icon().availableSizes())
            largest = qMax(largest, s.width());
        if (largest < size) { // no large image: enlarge the small one (also
            const QIcon up = upscaledIcon(b->icon(), size); // after Hwp
            b->setIcon(up);                                 // changes it)
            b->setProperty("_hwpfp_icon", up.cacheKey());
        } else {
            b->setProperty("_hwpfp_icon", b->icon().cacheKey());
        }
    }
    for (QScrollArea *area : toolbar->findChildren<QScrollArea *>()) {
        QWidget *content = area->widget();
        if (!content)
            continue;
        // fixed-width boxes (font size, ...) narrower than their contents
        for (QObject *o : content->children()) {
            if (!o->isWidgetType())
                continue;
            auto *box = static_cast<QWidget *>(o);
            if (box->minimumWidth() != box->maximumWidth() || !box->isVisible())
                continue;
            int need = 0;
            for (QObject *c : box->children())
                if (c->isWidgetType() && static_cast<QWidget *>(c)->isVisible()) {
                    auto *child = static_cast<QWidget *>(c);
                    need = qMax(need, child->x() + child->sizeHint().width() + child->x());
                }
            if (need > box->width()) {
                box->setFixedWidth(need);
                if (box->layout())
                    box->layout()->activate();
                else
                    for (QObject *c : box->children())
                        if (c->isWidgetType() && static_cast<QWidget *>(c)->isVisible())
                            static_cast<QWidget *>(c)->resize(need - 2 * static_cast<QWidget *>(c)->x(),
                                                              static_cast<QWidget *>(c)->height());
            }
        }
        // the row's fixed size was computed for 16 px icons
        content->setMinimumSize(0, 0);
        content->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        if (content->layout())
            content->layout()->activate();
        const QSize hint = content->sizeHint();
        content->setFixedSize(hint);
        area->setMinimumHeight(hint.height());
        area->setMaximumHeight(hint.height());
    }
    toolbar->updateGeometry();
}

// The recent-file list in the 파일 menu (and the 불러오기 drop-down) reports
// its real height only after the menu was laid out.
void fixMenuWidget(QWidget *list)
{
    QWidget *wrapper = list->parentWidget();
    if (!wrapper)
        return;
    const int need = list->sizeHint().height();
    if (need <= wrapper->height() && need <= list->height())
        return;
    list->setMinimumHeight(need);
    wrapper->setMinimumHeight(need);
    auto *menu = qobject_cast<QMenu *>(wrapper->parentWidget());
    if (!menu)
        return;
    for (QAction *a : menu->actions())
        if (qobject_cast<QWidgetAction *>(a)) { // QMenu re-measures its items
            QActionEvent changed(QEvent::ActionChanged, a);
            notifyObject(menu, &changed);
        }
}

class AppFilter : public QObject
{
public:
    bool eventFilter(QObject *obj, QEvent *ev) override
    {
        const QEvent::Type type = ev->type();
        if (type == QEvent::Wheel)
            return wheel(obj, ev);
        if (!uiFixes() || !obj->isWidgetType())
            return false;
        if (type == QEvent::Show || type == QEvent::LayoutRequest) {
            if (isClass(obj, "hanul::RecentFileListWidget"))
                later(obj, [](QWidget *w) { fixMenuWidget(w); });
            else if (type == QEvent::Show && isClass(obj, "hanul::ToolBarImpl"))
                later(obj, [](QWidget *w) { fixToolbar(w); });
        } else if (type == QEvent::Paint && toolbarIconSize()
                   && qobject_cast<QToolButton *>(obj)) {
            auto *b = static_cast<QToolButton *>(obj);
            const int size = toolbarIconSize();
            if (b->property("_hwpfp_icon").isValid()
                && (b->iconSize() != QSize(size, size)
                    || b->icon().cacheKey() != b->property("_hwpfp_icon").toLongLong())) {
                // Hwp replaced the icon (e.g. the text colour button) or the size
                for (QWidget *p = b->parentWidget(); p; p = p->parentWidget())
                    if (isClass(p, "hanul::ToolBarImpl")) {
                        later(p, [](QWidget *w) { fixToolbar(w); });
                        break;
                    }
            }
        }
        return false;
    }

private:
    template <typename F> void later(QObject *obj, F fix)
    {
        QPointer<QWidget> w(static_cast<QWidget *>(obj));
        if (scheduled.contains(obj))
            return;
        scheduled.insert(obj);
        QTimer::singleShot(0, this, [this, w, obj, fix] {
            scheduled.remove(obj);
            if (w)
                fix(w.data());
        });
    }
    QSet<QObject *> scheduled; // fixes queued for after the current event

    // ---- wheel event coalescing ----
    bool wheel(QObject *obj, QEvent *ev)
    {
        if (injecting || !fastScroll() || hashMode() || !obj->isWidgetType()
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

public:
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
        injecting = true;
        notifyObject(target, &merged);
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

void installAppFilter()
{
    static bool done;
    if (done || !QCoreApplication::instance() || (!fastScroll() && !uiFixes()))
        return;
    done = true;
    QCoreApplication::instance()->installEventFilter(new AppFilter);
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
    installAppFilter(); // first call happens once the application runs
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
