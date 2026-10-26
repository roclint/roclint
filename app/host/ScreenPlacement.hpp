#pragma once

#include <QPoint>
#include <QRect>

namespace roclint::host {

struct PlacementResult {
    int x{0};
    int y{0};
    bool clamped{false};
};

class ScreenPlacement {
public:
    // Pure calculation function (testable with mock geometry)
    static PlacementResult calculateClampedPosition(
        const QPoint& cursorPos,
        int cardWidth,
        int cardHeight,
        const QRect& availableGeometry,
        int margin = 12
    );

    // Anchors a widget (e.g. corner badge or floating card) relative to a target prompt box bounding rect
    static PlacementResult calculateAnchorPosition(
        const QRect& targetPromptRect,
        int widgetWidth,
        int widgetHeight,
        const QRect& availableGeometry,
        int margin = 12
    );

    // Determines real-time cursor screen and placement
    static PlacementResult determineTargetPosition(int cardWidth, int cardHeight);
    static PlacementResult determineAnchorPosition(const QRect& targetPromptRect, int widgetWidth, int widgetHeight, int margin = 12);
};

} // namespace roclint::host
