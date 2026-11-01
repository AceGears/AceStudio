#pragma once
// Interface motion: one place that decides how long (and whether) things
// animate. Reduced motion (the setting, or Windows' "Animation effects" off)
// makes every duration 0, so transitions complete immediately.

class QWidget;

namespace acestudio::ui::motion {

void configure(bool reducedMotion, int speedPercent);
bool reduced();
// `baseMs` scaled by the speed setting, or 0 with reduced motion.
int duration(int baseMs);
// Windows' Settings › Accessibility › Visual effects › Animation effects.
bool systemAnimationsOff();

// Fades `w` in from transparent (no-op with reduced motion).
void fadeIn(QWidget* w, int baseMs = 180);

} // namespace acestudio::ui::motion
