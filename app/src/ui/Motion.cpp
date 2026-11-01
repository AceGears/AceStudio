#include "Motion.h"

#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QWidget>

#include <algorithm>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace acestudio::ui::motion {

namespace {
bool g_reduced = false;
int g_speed = 100;
} // namespace

bool systemAnimationsOff() {
#ifdef _WIN32
    BOOL on = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0)) return !on;
#endif
    return false;
}

void configure(bool reducedMotion, int speedPercent) {
    g_reduced = reducedMotion || systemAnimationsOff();
    g_speed = std::clamp(speedPercent, 25, 400);
}

bool reduced() { return g_reduced; }

int duration(int baseMs) { return g_reduced ? 0 : baseMs * g_speed / 100; }

void fadeIn(QWidget* w, int baseMs) {
    const int ms = duration(baseMs);
    if (!w || ms <= 0) return;
    auto* effect = new QGraphicsOpacityEffect(w);
    effect->setOpacity(0.0);
    w->setGraphicsEffect(effect);
    auto* anim = new QPropertyAnimation(effect, "opacity", w);
    anim->setDuration(ms);
    anim->setStartValue(0.0);
    anim->setEndValue(1.0);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    // Remove the effect afterwards: an opacity effect renders the widget
    // through an offscreen pixmap, which costs time on every repaint.
    QObject::connect(anim, &QPropertyAnimation::finished, w, [w] { w->setGraphicsEffect(nullptr); });
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

} // namespace acestudio::ui::motion
