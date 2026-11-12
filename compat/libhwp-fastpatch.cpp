// libhwp-fastpatch.so: fast, partial repaints for Hancom's libhsp.so (Hwp).
//
// libhsp.so (Hancom's Win32 emulation layer) keeps a back-buffer QImage per
// window. When a DC is created it stores a fingerprint of the whole image
// (an MD5 in 11.20.0.1520); when the DC is released it fingerprints the image
// again and, if the two differ, calls QWidget::update(): a repaint of the
// *whole* window (blend + clear + X11 upload of ~15 MB on a 2540x1500
// window) for every keystroke. "No change -> no update" is also what stops
// libhsp's repaint -> WM_PAINT -> repaint loop, so the check has to stay.
//
// patch-libhsp.py makes libhsp's fingerprint function tail-call
// hwp_fast_fingerprint() (rdi = QByteArray *result, rsi = window holder with
// the QWidget * at +0x10 and the QImage at +0x18). This function
//   * keeps a copy of each window image plus a 64-bit XXH3 hash per row,
//   * on DC release hashes the rows (in parallel), compares them with the
//     stored hashes, finds the changed spans of changed rows, updates the
//     copy, and calls QWidget::update(region) for the changed rectangles only,
//   * always returns the same token, so libhsp's own full update() never runs.
//
// Environment:
//   HWP_REPAINT=hash          whole-window updates with an XXH3 fingerprint
//                             (libhsp's original logic, only faster)
//   HWP_FASTPATCH_THREADS=N   threads for row hashing (default: min(4, CPUs))
//   HWP_FASTPATCH_DUMP=DIR    debug: dump the back buffer (raw BGRA) to DIR
//                             on every DC release
#include <QtCore/QByteArray>
#include <QtGui/QImage>
#include <QtGui/QRegion>
#include <QtWidgets/QWidget>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace {

// Return addresses of the two fingerprint calls inside libhsp.so 11.20.0.1520
// (checked by patch-libhsp.py).
constexpr uintptr_t kStartCall = 0x16b06e; // after CreateDCW
constexpr uintptr_t kEndCall = 0x16a6ac;   // DC release: compare + update()

struct Snapshot {
    const uchar *bits = nullptr;
    int width = 0, height = 0;
    qsizetype bytesPerLine = 0;
    QImage::Format format = QImage::Format_Invalid;
    std::vector<uchar> data;            // copy of the image
    std::vector<XXH64_hash_t> rowHash;  // XXH3 of each row of `data`
};

std::mutex lock;
std::unordered_map<const void *, Snapshot> snapshots;
std::vector<XXH64_hash_t> rowHashNow;
uintptr_t hspBase;

const char *env(const char *name)
{
    const char *v = getenv(name);
    return v && *v ? v : nullptr;
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

void hashRows(const uchar *bits, qsizetype bpl, qsizetype rowBytes, int h, XXH64_hash_t *out)
{
    auto run = [=](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            out[y] = XXH3_64bits(bits + y * bpl, size_t(rowBytes));
    };
    const int parts = qMin(threads(), qMax(1, h / 64));
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
        return QRegion(0, 0, w, h);
    }
    rowHashNow.resize(h);
    hashRows(bits, bpl, rowBytes, h, rowHashNow.data());

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

void dumpImage(const void *holder, const QImage *img, const char *dir)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%dx%d-%p.raw", dir, img->width(), img->height(), holder);
    if (FILE *f = fopen(path, "wb")) {
        for (int y = 0; y < img->height(); ++y)
            fwrite(img->constScanLine(y), size_t(img->depth() / 8), size_t(img->width()), f);
        fclose(f);
    }
}

} // namespace

extern "C" __attribute__((visibility("default"))) QByteArray hwp_fast_fingerprint(const char *holder)
{
    static const QByteArray token("hwp-fastpatch");
    const QImage *img = reinterpret_cast<const QImage *>(holder + 0x18);
    const uintptr_t at = callerOffset(__builtin_return_address(0));
    if (hashMode() || (at != kStartCall && at != kEndCall))
        return xxh3(img); // libhsp's own logic: compare fingerprints, full update()
    if (at == kStartCall)
        return token;

    std::lock_guard<std::mutex> guard(lock);
    QWidget *widget = *reinterpret_cast<QWidget *const *>(holder + 0x10);
    const QRegion dirty = diffAndSync(holder, img);
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
