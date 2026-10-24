#include "ScreenPlacement.hpp"

#include <QGuiApplication>
#include <QScreen>
#include <QCursor>

namespace roclint::host {

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

} // namespace roclint::host
