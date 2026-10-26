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
