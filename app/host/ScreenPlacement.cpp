#include "ScreenPlacement.hpp"

#include <QGuiApplication>
#include <QScreen>
#include <QCursor>
#include <QtMath>
#include <QDebug>
#include <algorithm>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace roclint::host {

PlacementResult ScreenPlacement::calculateIndicatorPosition(const QRect& field, const QRect& availableGeometry) {
    constexpr int size = 28, gap = 6, margin = 8;
    if (field.isEmpty() || availableGeometry.isEmpty()) return {100, 100, false};
    const QRect safe = availableGeometry.adjusted(margin, margin, -margin, -margin);
    // Prefer outside the editor so the indicator cannot cover its Send/control row.
    const QPoint candidates[] = {
        {field.right() + gap + 1, field.bottom() - size + 1},
        {field.right() - size + 1, field.bottom() + gap + 1},
        {field.right() - size + 1, field.top() - gap - size},
        {field.left() - gap - size, field.bottom() - size + 1}
    };
    for (const auto& point : candidates)
        if (safe.contains(QRect(point, QSize(size, size)))) return {point.x(), point.y(), false};
    // A screen-filling editor can leave no external space. Keep the tiny control
    // on-screen at its edge; never move it to the middle of the draft.
    return {std::clamp(field.right() - size + 1, safe.left(), std::max(safe.left(), safe.right() - size + 1)),
            std::clamp(field.bottom() - size + 1, safe.top(), std::max(safe.top(), safe.bottom() - size + 1)), true};
}

PlacementResult ScreenPlacement::determineIndicatorPosition(const QRect& field) {
    auto* screen = QGuiApplication::screenAt(field.center());
    if (!screen) screen = QGuiApplication::primaryScreen();
    return calculateIndicatorPosition(field, screen ? screen->availableGeometry() : QRect());
}

QRect ScreenPlacement::mapNativeRect(const QRect& rect, const QRect& nativeScreen, const QRect& logicalScreen) {
    if (rect.isEmpty() || nativeScreen.isEmpty() || logicalScreen.isEmpty()) return {};
    const double sx = static_cast<double>(logicalScreen.width()) / nativeScreen.width();
    const double sy = static_cast<double>(logicalScreen.height()) / nativeScreen.height();
    const int x = logicalScreen.x() + qFloor((static_cast<double>(rect.x()) - nativeScreen.x()) * sx);
    const int y = logicalScreen.y() + qFloor((static_cast<double>(rect.y()) - nativeScreen.y()) * sy);
    const int endX = logicalScreen.x() + qCeil((static_cast<double>(rect.x()) + rect.width() - nativeScreen.x()) * sx);
    const int endY = logicalScreen.y() + qCeil((static_cast<double>(rect.y()) + rect.height() - nativeScreen.y()) * sy);
    return QRect(x, y, std::max(1, endX - x), std::max(1, endY - y));
}

QRect ScreenPlacement::fromNativePixels(const QRect& rect) {
    if (rect.isEmpty()) return {};
#ifdef Q_OS_WIN
    const RECT bounds{rect.x(), rect.y(), rect.x() + rect.width(), rect.y() + rect.height()};
    const auto monitor = MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&info))) return {};
    const auto device = QString::fromWCharArray(info.szDevice);
    const auto deviceName = [](QString name) {
        if (name.startsWith(QStringLiteral("\\\\.\\"))) name.remove(0, 4);
        return name;
    };
    for (const auto* screen : QGuiApplication::screens()) {
        if (deviceName(screen->name()).compare(deviceName(device), Qt::CaseInsensitive) != 0) continue;
        const auto& native = info.rcMonitor;
        const auto mapped = mapNativeRect(rect, QRect(native.left, native.top, native.right - native.left,
                                        native.bottom - native.top), screen->geometry());
        if (qEnvironmentVariableIsSet("ROCLINT_CAPTURE_DIAGNOSTICS"))
            qInfo() << "[Roclint geometry] monitor=" << device << "native=" << rect << "logical=" << mapped;
        return mapped;
    }
    // Unknown monitor mapping uses the existing unknown-anchor fallback, not a
    // guess that applies one display's scale to another display's coordinates.
    return {};
#else
    return rect;
#endif
}

PlacementResult ScreenPlacement::calculateClampedPosition(
    const QPoint& cursorPos,
    int cardWidth,
    int cardHeight,
    const QRect& availableGeometry,
    int margin
) {
    PlacementResult result;

    if (availableGeometry.isEmpty()) {
        result.x = 100;
        result.y = 100;
        result.clamped = false;
        return result;
    }

    // Default target: centered horizontally near cursor, vertically offset slightly
    int targetX = cursorPos.x() - (cardWidth / 2);
    int targetY = cursorPos.y() - 60;

    const int minX = availableGeometry.left() + margin;
    const int maxX = availableGeometry.left() + availableGeometry.width() - cardWidth - margin;
    const int minY = availableGeometry.top() + margin;
    const int maxY = availableGeometry.top() + availableGeometry.height() - cardHeight - margin;

    bool clamped = false;

    // Clamp horizontally
    if (maxX < minX) {
        // Screen is narrower than card + margins
        targetX = availableGeometry.left();
        clamped = true;
    } else if (targetX < minX) {
        targetX = minX;
        clamped = true;
    } else if (targetX > maxX) {
        targetX = maxX;
        clamped = true;
    }

    // Clamp vertically
    if (maxY < minY) {
        // Screen is shorter than card + margins
        targetY = availableGeometry.top();
        clamped = true;
    } else if (targetY < minY) {
        targetY = minY;
        clamped = true;
    } else if (targetY > maxY) {
        targetY = maxY;
        clamped = true;
    }

    result.x = targetX;
    result.y = targetY;
    result.clamped = clamped;
    return result;
}

PlacementResult ScreenPlacement::calculateAnchorPosition(
    const QRect& targetPromptRect,
    int widgetWidth,
    int widgetHeight,
    const QRect& availableGeometry,
    int margin
) {
    if (availableGeometry.isEmpty()) {
        return PlacementResult{100, 100, false};
    }
    if (targetPromptRect.isEmpty() || !targetPromptRect.isValid()) {
        const QPoint center(availableGeometry.left() + availableGeometry.width() / 2,
                            availableGeometry.top() + availableGeometry.height() / 2);
        return calculateClampedPosition(center, widgetWidth, widgetHeight, availableGeometry, margin);
    }

    int targetX = 0;
    int targetY = 0;

    // Grammarly-style: for compact badges (e.g. height <= 40), float inside the bottom-right corner
    // of the prompt box if the box is large enough; otherwise float just outside/below the bottom-right.
    if (widgetHeight <= 40 && targetPromptRect.width() >= widgetWidth + 16 && targetPromptRect.height() >= widgetHeight + 10) {
        targetX = targetPromptRect.right() - widgetWidth - 6;
        targetY = targetPromptRect.bottom() - widgetHeight - 4;
    } else if (widgetHeight <= 40) {
        targetX = targetPromptRect.right() - widgetWidth;
        targetY = targetPromptRect.bottom() + 4;
    } else {
        // Larger review card (e.g. FloatingCard.qml):
        // Align horizontally near the right of the prompt box if the prompt box is wide, else left-aligned
        if (targetPromptRect.width() >= widgetWidth) {
            targetX = targetPromptRect.right() - widgetWidth;
        } else {
            targetX = targetPromptRect.left();
        }

        // Prefer placing above the prompt box (Grammarly-style) if space permits:
        if (targetPromptRect.top() - widgetHeight - 8 >= availableGeometry.top() + margin) {
            targetY = targetPromptRect.top() - widgetHeight - 8;
        } else {
            // Otherwise place below the prompt box:
            targetY = targetPromptRect.bottom() + 8;
        }
    }

    const int minX = availableGeometry.left() + margin;
    const int maxX = availableGeometry.left() + availableGeometry.width() - widgetWidth - margin;
    const int minY = availableGeometry.top() + margin;
    const int maxY = availableGeometry.top() + availableGeometry.height() - widgetHeight - margin;

    bool clamped = false;
    if (maxX < minX) {
        targetX = availableGeometry.left();
        clamped = true;
    } else if (targetX < minX) {
        targetX = minX;
        clamped = true;
    } else if (targetX > maxX) {
        targetX = maxX;
        clamped = true;
    }

    if (maxY < minY) {
        targetY = availableGeometry.top();
        clamped = true;
    } else if (targetY < minY) {
        targetY = minY;
        clamped = true;
    } else if (targetY > maxY) {
        targetY = maxY;
        clamped = true;
    }

    return PlacementResult{targetX, targetY, clamped};
}

PlacementResult ScreenPlacement::calculateReviewPosition(const QRect& field, const QSize& size, const QRect& workArea) {
    if (workArea.isEmpty()) return {100, 100, false};

    constexpr int margin = 12;
    constexpr int gap = 10;
    const auto safe = workArea.adjusted(margin, margin, -margin, -margin);
    const int maxX = safe.right() - size.width() + 1;
    const int maxY = safe.bottom() - size.height() + 1;
    const auto clamp = [&](const QPoint& point) {
        return QPoint(
            maxX < safe.left() ? workArea.left() : std::clamp(point.x(), safe.left(), maxX),
            maxY < safe.top() ? workArea.top() : std::clamp(point.y(), safe.top(), maxY));
    };

    const QPoint dock = clamp(QPoint(maxX, safe.top()));
    if (field.isEmpty() || !workArea.intersects(field)) return {dock.x(), dock.y(), false};

    const int alignedX = field.right() - size.width() + 1;
    const QPoint candidates[] = {
        {alignedX, field.top() - size.height() - gap},
        {field.right() + gap + 1, field.bottom() - size.height() + 1},
        {field.left() - size.width() - gap, field.bottom() - size.height() + 1},
        {alignedX, field.bottom() + gap + 1},
    };
    const auto overlap = [&](const QPoint& point) {
        const QRect intersection = QRect(point, size).intersected(field);
        return qint64(intersection.width()) * intersection.height();
    };

    QPoint best = dock;
    for (const QPoint& candidate : candidates) {
        const QPoint point = clamp(candidate);
        if (overlap(point) == 0) return {point.x(), point.y(), point != candidate};
        if (overlap(point) < overlap(best)) best = point;
    }
    return {best.x(), best.y(), true};
}

PlacementResult ScreenPlacement::determineReviewPosition(const QRect& field, const QSize& size) {
    auto* screen = field.isEmpty() ? nullptr : QGuiApplication::screenAt(field.center());
    if (!screen) screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen) screen = QGuiApplication::primaryScreen();
    return calculateReviewPosition(field, size, screen ? screen->availableGeometry() : QRect());
}

PlacementResult ScreenPlacement::determineTargetPosition(int cardWidth, int cardHeight) {
    const QPoint cursorPos = QCursor::pos();
    QScreen* targetScreen = QGuiApplication::screenAt(cursorPos);

    if (!targetScreen) {
        targetScreen = QGuiApplication::primaryScreen();
    }

    if (!targetScreen) {
        return PlacementResult{100, 100, false};
    }

    const QRect avail = targetScreen->availableGeometry();
    if (cursorPos.isNull() || !targetScreen->geometry().contains(cursorPos)) {
        PlacementResult result;
        result.x = avail.left() + std::max(0, (avail.width() - cardWidth) / 2);
        result.y = avail.top() + std::max(0, (avail.height() - cardHeight) / 2);
        result.clamped = false;
        return result;
    }

    return calculateClampedPosition(cursorPos, cardWidth, cardHeight, avail);
}

PlacementResult ScreenPlacement::determineAnchorPosition(const QRect& targetPromptRect, int widgetWidth, int widgetHeight, int margin) {
    if (targetPromptRect.isEmpty() || !targetPromptRect.isValid()) {
        return determineTargetPosition(widgetWidth, widgetHeight);
    }

    QScreen* targetScreen = QGuiApplication::screenAt(targetPromptRect.center());
    if (!targetScreen) {
        targetScreen = QGuiApplication::screenAt(QCursor::pos());
    }
    if (!targetScreen) {
        targetScreen = QGuiApplication::primaryScreen();
    }
    if (!targetScreen) {
        return PlacementResult{100, 100, false};
    }

    return calculateAnchorPosition(targetPromptRect, widgetWidth, widgetHeight, targetScreen->availableGeometry(), margin);
}

} // namespace roclint::host
