// The bundled QtWebEngine 5.11 imports a few libstdc++ functions
// (std::__throw_out_of_range_fmt, sized operator delete / delete[]) with the
// symbol version Qt_5: the bundled Qt 5.11 libraries (Qml, Positioning) were
// linked with a static libstdc++ and re-export it under that version.
// nixpkgs Qt 5.15 does not, so this shim provides the versioned symbols when
// QtWebEngine 5.11 runs on top of Qt 5.15 (fcitx5 mode).
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <stdexcept>

extern "C" [[noreturn]] void hnc_throw_out_of_range_fmt(const char *fmt, ...)
    __asm__("_ZSt24__throw_out_of_range_fmtPKcz");

extern "C" void hnc_throw_out_of_range_fmt(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    throw std::out_of_range(buf);
}

extern "C" void hnc_delete_sized(void *p, std::size_t) __asm__("_ZdlPvm");
extern "C" void hnc_delete_sized(void *p, std::size_t) { ::operator delete(p); }

extern "C" void hnc_delete_array_sized(void *p, std::size_t) __asm__("_ZdaPvm");
extern "C" void hnc_delete_array_sized(void *p, std::size_t) { ::operator delete[](p); }
