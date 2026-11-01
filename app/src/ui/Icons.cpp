#include "Icons.h"

#include "../Theme.h"

#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QtMath>

namespace acestudio::ui {

namespace {

// Strokes of 1.75 units on a 24-unit grid, round caps and joins: the same
// weight as the interface text at normal sizes.
void drawGlyph(QPainter& p, Glyph g, const QColor& c) {
    QPen pen(c, 1.75, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    auto fillDot = [&](QPointF at, qreal r) {
        p.save();
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawEllipse(at, r, r);
        p.restore();
    };
    auto sparkle = [&](QPointF ctr, qreal r) {   // four-point star with concave sides
        QPainterPath s;
        s.moveTo(ctr.x(), ctr.y() - r);
        s.quadTo(ctr.x(), ctr.y(), ctr.x() + r, ctr.y());
        s.quadTo(ctr.x(), ctr.y(), ctr.x(), ctr.y() + r);
        s.quadTo(ctr.x(), ctr.y(), ctr.x() - r, ctr.y());
        s.quadTo(ctr.x(), ctr.y(), ctr.x(), ctr.y() - r);
        p.save();
        p.setBrush(c);
        p.drawPath(s);
        p.restore();
    };
    switch (g) {
    case Glyph::Home: {
        QPainterPath h;
        h.moveTo(3.5, 11);
        h.lineTo(12, 4);
        h.lineTo(20.5, 11);
        p.drawPath(h);
        p.drawRoundedRect(QRectF(6, 10, 12, 10), 1.5, 1.5);
        p.drawLine(QPointF(10.5, 20), QPointF(10.5, 15));
        p.drawLine(QPointF(13.5, 20), QPointF(13.5, 15));
        p.drawLine(QPointF(10.5, 15), QPointF(13.5, 15));
        break;
    }
    case Glyph::AiStudio:
    case Glyph::Sparkle:
        sparkle({10, 13}, 7);
        sparkle({18, 5.5}, 3);
        sparkle({18.5, 17.5}, 2.2);
        break;
    case Glyph::Characters:
        p.drawEllipse(QPointF(12, 8), 3.6, 3.6);
        {
            QPainterPath b;
            b.moveTo(4.5, 20.5);
            b.cubicTo(5, 15, 8, 13.5, 12, 13.5);
            b.cubicTo(16, 13.5, 19, 15, 19.5, 20.5);
            p.drawPath(b);
        }
        break;
    case Glyph::Animation:
        p.drawRoundedRect(QRectF(3, 5, 18, 14), 2, 2);
        for (qreal x : {7.0, 12.0, 17.0}) p.drawLine(QPointF(x, 5), QPointF(x, 19));
        fillDot({4.9, 8}, 0.9);
        fillDot({4.9, 16}, 0.9);
        break;
    case Glyph::World: {
        QPainterPath m;
        m.moveTo(2.5, 19.5);
        m.lineTo(9, 9);
        m.lineTo(13, 15);
        m.lineTo(16, 11);
        m.lineTo(21.5, 19.5);
        m.closeSubpath();
        p.drawPath(m);
        p.drawEllipse(QPointF(17.5, 5.5), 1.8, 1.8);
        break;
    }
    case Glyph::Library:
        p.drawRoundedRect(QRectF(3, 7, 12, 13), 1.5, 1.5);
        p.drawPolyline(QPolygonF({QPointF(6, 4), QPointF(18, 4), QPointF(18, 17)}));
        p.drawPolyline(QPolygonF({QPointF(9, 1.5), QPointF(21, 1.5), QPointF(21, 14)}));
        break;
    case Glyph::Editor: {
        // A pencil over a 2x2 pixel block.
        p.save();
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRect(QRectF(3, 16, 2.5, 2.5));
        p.drawRect(QRectF(6, 18.5, 2.5, 2.5));
        p.restore();
        QPainterPath pen2;
        pen2.moveTo(8.5, 15.5);
        pen2.lineTo(17, 7);
        pen2.lineTo(20, 10);
        pen2.lineTo(11.5, 18.5);
        pen2.closeSubpath();
        p.drawPath(pen2);
        p.drawLine(QPointF(15, 9), QPointF(18, 12));
        break;
    }
    case Glyph::Export:
        p.drawPolyline(QPolygonF({QPointF(8, 9), QPointF(5, 9), QPointF(5, 20), QPointF(19, 20), QPointF(19, 9), QPointF(16, 9)}));
        p.drawLine(QPointF(12, 15), QPointF(12, 3));
        p.drawPolyline(QPolygonF({QPointF(8.5, 6.5), QPointF(12, 3), QPointF(15.5, 6.5)}));
        break;
    case Glyph::Integrations:
        p.drawRoundedRect(QRectF(3, 3, 7, 7), 2, 2);
        p.drawRoundedRect(QRectF(14, 14, 7, 7), 2, 2);
        p.drawEllipse(QPointF(17.5, 6.5), 3.3, 3.3);
        p.drawLine(QPointF(10, 6.5), QPointF(14.2, 6.5));
        p.drawLine(QPointF(17.5, 9.8), QPointF(17.5, 14));
        p.drawLine(QPointF(6.5, 10), QPointF(6.5, 17.5));
        p.drawLine(QPointF(6.5, 17.5), QPointF(14, 17.5));
        break;
    case Glyph::Settings: {
        const QPointF ctr(12, 12);
        QPainterPath gear;
        const int teeth = 8;
        for (int i = 0; i < teeth * 2; ++i) {
            const qreal a = i * M_PI / teeth;
            const qreal r = (i % 2) ? 6.8 : 8.6;
            const QPointF pt(ctr.x() + r * qCos(a), ctr.y() + r * qSin(a));
            if (i == 0) gear.moveTo(pt);
            else gear.lineTo(pt);
        }
        gear.closeSubpath();
        p.drawPath(gear);
        p.drawEllipse(ctr, 2.8, 2.8);
        break;
    }
    case Glyph::New:
    case Glyph::Plus:
        if (g == Glyph::New) p.drawRoundedRect(QRectF(4, 3, 16, 18), 2, 2);
        p.drawLine(QPointF(12, 8), QPointF(12, 16));
        p.drawLine(QPointF(8, 12), QPointF(16, 12));
        break;
    case Glyph::Open:
    case Glyph::Folder: {
        QPainterPath f;
        f.moveTo(3, 19);
        f.lineTo(3, 6);
        f.lineTo(9, 6);
        f.lineTo(11, 8);
        f.lineTo(21, 8);
        f.lineTo(21, 19);
        f.closeSubpath();
        p.drawPath(f);
        if (g == Glyph::Open) p.drawLine(QPointF(3, 12), QPointF(21, 12));
        break;
    }
    case Glyph::Play: {
        QPainterPath t;
        t.moveTo(8, 5);
        t.lineTo(19, 12);
        t.lineTo(8, 19);
        t.closeSubpath();
        p.save();
        p.setBrush(c);
        p.drawPath(t);
        p.restore();
        break;
    }
    case Glyph::Search:
        p.drawEllipse(QPointF(10.5, 10.5), 6, 6);
        p.drawLine(QPointF(15, 15), QPointF(20.5, 20.5));
        break;
    case Glyph::Star: {
        QPainterPath s;
        for (int i = 0; i < 10; ++i) {
            const qreal a = -M_PI / 2 + i * M_PI / 5;
            const qreal r = (i % 2) ? 3.8 : 8.8;
            const QPointF pt(12 + r * qCos(a), 12.5 + r * qSin(a));
            if (i == 0) s.moveTo(pt);
            else s.lineTo(pt);
        }
        s.closeSubpath();
        p.drawPath(s);
        break;
    }
    case Glyph::Tag: {
        QPainterPath t;
        t.moveTo(3, 3);
        t.lineTo(12, 3);
        t.lineTo(21, 12);
        t.lineTo(12, 21);
        t.lineTo(3, 12);
        t.closeSubpath();
        p.drawPath(t);
        fillDot({8, 8}, 1.4);
        break;
    }
    case Glyph::Grid:
        for (qreal x : {3.0, 13.0})
            for (qreal y : {3.0, 13.0}) p.drawRoundedRect(QRectF(x, y, 8, 8), 1.5, 1.5);
        break;
    case Glyph::Image:
        p.drawRoundedRect(QRectF(3, 4, 18, 16), 2, 2);
        p.drawEllipse(QPointF(9, 9.5), 1.8, 1.8);
        p.drawPolyline(QPolygonF({QPointF(3.5, 18), QPointF(10, 12.5), QPointF(14, 16), QPointF(16.5, 13.5), QPointF(20.5, 17.5)}));
        break;
    case Glyph::Check:
        p.drawPolyline(QPolygonF({QPointF(5, 12.5), QPointF(10, 17.5), QPointF(19.5, 7)}));
        break;
    case Glyph::Warning: {
        QPainterPath t;
        t.moveTo(12, 3.5);
        t.lineTo(21, 19.5);
        t.lineTo(3, 19.5);
        t.closeSubpath();
        p.drawPath(t);
        p.drawLine(QPointF(12, 9.5), QPointF(12, 13.5));
        fillDot({12, 16.6}, 1.1);
        break;
    }
    case Glyph::Trash:
        p.drawLine(QPointF(4, 6.5), QPointF(20, 6.5));
        p.drawPolyline(QPolygonF({QPointF(9, 6.5), QPointF(9.5, 3.5), QPointF(14.5, 3.5), QPointF(15, 6.5)}));
        p.drawPolyline(QPolygonF({QPointF(6, 6.5), QPointF(7, 20.5), QPointF(17, 20.5), QPointF(18, 6.5)}));
        break;
    case Glyph::Refresh: {
        QPainterPath a;
        a.arcMoveTo(QRectF(4.5, 4.5, 15, 15), 60);
        a.arcTo(QRectF(4.5, 4.5, 15, 15), 60, 280);
        p.drawPath(a);
        p.drawPolyline(QPolygonF({QPointF(15.5, 2.5), QPointF(16, 6.2), QPointF(19.7, 5.7)}));
        break;
    }
    }
}

class GlyphEngine final : public QIconEngine {
public:
    explicit GlyphEngine(Glyph g) : m_glyph(g) {}
    void paint(QPainter* p, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
        const ThemeTokens& t = themeTokens();
        QColor c = t.textMuted.isValid() ? t.textMuted : QColor("#8C95A7");
        if (mode == QIcon::Disabled) c = t.disabled;
        else if (mode == QIcon::Selected || state == QIcon::On) c = t.accentHi.isValid() ? t.accentHi : QColor("#4D9BFF");
        else if (mode == QIcon::Active) c = t.text;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const qreal s = qMin(rect.width(), rect.height()) / 24.0;
        p->translate(rect.x() + (rect.width() - 24 * s) / 2, rect.y() + (rect.height() - 24 * s) / 2);
        p->scale(s, s);
        drawGlyph(*p, m_glyph, c);
        p->restore();
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        QPixmap pm(size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        paint(&p, QRect(QPoint(0, 0), size), mode, state);
        return pm;
    }
    QIconEngine* clone() const override { return new GlyphEngine(m_glyph); }

private:
    Glyph m_glyph;
};

} // namespace

QIcon icon(Glyph g) { return QIcon(new GlyphEngine(g)); }

} // namespace acestudio::ui
