// Hwp's title bar drag calls a Qt 5.11 private overload:
//   void QWindowSystemInterface::handleMouseEvent<DefaultDelivery>(
//       QWindow *, const QPointF &, const QPointF &, Qt::MouseButtons,
//       Qt::KeyboardModifiers, Qt::MouseEventSource)
// Qt 5.15 still has that overload, but it returns bool, so its mangled name
// differs and Hwp exits with "symbol lookup error" on the first drag. This
// shim exports the Qt 5.11 name (version Qt_5_PRIVATE_API) and forwards to
// the Qt 5.15 overload, which deduces the button and event type the same way.
#include <qpa/qwindowsysteminterface.h>

extern "C" void hnc_handle_mouse_event(QWindow *window, const QPointF &local,
                                       const QPointF &global, Qt::MouseButtons buttons,
                                       Qt::KeyboardModifiers mods,
                                       Qt::MouseEventSource source)
    __asm__("_ZN22QWindowSystemInterface16handleMouseEventINS_15DefaultDeliveryEEEvP7QWindowRK7QPointFS6_6QFlagsIN2Qt11MouseButtonEES7_INS8_16KeyboardModifierEENS8_16MouseEventSourceE");

extern "C" void hnc_handle_mouse_event(QWindow *window, const QPointF &local,
                                       const QPointF &global, Qt::MouseButtons buttons,
                                       Qt::KeyboardModifiers mods,
                                       Qt::MouseEventSource source)
{
    QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::DefaultDelivery>(
        window, local, global, buttons, mods, source);
}
