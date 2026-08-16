#include "Banner.hpp"

#include <QFont>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>

namespace
{
    /// 164x314 is the size InstallShield used and every installer since has
    /// copied; ours is taller because a Qt wizard page with a check list and a
    /// stage grid in it is taller than a 1997 dialog.
    constexpr int kWatermarkWidth = 180;
    constexpr int kWatermarkHeight = 420;

    /// THE BANNER'S WIDTH IS THE WIZARD'S WIDTH. QWizard sizes the header to
    /// this pixmap rather than tiling it to fit, so the number here is not a
    /// drawing detail -- a 1600-pixel strip opened a 1600-pixel window, and a
    /// 1-pixel one collapsed the whole wizard to a column. 720 is the width the
    /// pages actually want: the check list and the stage grid both fit without
    /// wrapping, and InstallWizard opens at the same figure so the window does
    /// not jump when the first header appears.
    constexpr int kBannerWidth = 720;
    constexpr int kBannerHeight = 64;

    constexpr int kLogoSize = 48;

    /// The gold ring. Riven's islands are lit by domes and this is the only
    /// shape in the game recognisable at 60 pixels across in a flat colour.
    void drawDome(QPainter &p, const QPointF &centre, qreal radius, qreal opacity)
    {
        p.save();
        p.setOpacity(opacity);
        p.setBrush(Qt::NoBrush);

        QPen pen(banner::kGold);
        pen.setWidthF(radius * 0.10);
        p.setPen(pen);
        p.drawEllipse(centre, radius, radius);

        // The seam across the middle, which is what makes it read as a dome
        // rather than as a circle.
        pen.setWidthF(radius * 0.05);
        p.setPen(pen);
        p.drawLine(QPointF(centre.x() - radius, centre.y()),
                   QPointF(centre.x() + radius, centre.y()));
        p.restore();
    }
} // namespace

namespace banner
{

QPixmap watermark()
{
    // Cached: QWizard asks for this on every page change, and repainting a
    // gradient and two ellipses each time would be pure waste.
    static QPixmap cached;
    if (!cached.isNull())
        return cached;

    QPixmap pm(kWatermarkWidth, kWatermarkHeight);
    pm.fill(kInk);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);

    // Sky into water. The horizon sits low, at two thirds, so the dome and the
    // title have room above it.
    const qreal horizon = kWatermarkHeight * 0.66;
    QLinearGradient sky(0, 0, 0, horizon);
    sky.setColorAt(0.0, kInk);
    sky.setColorAt(1.0, kDeep);
    p.fillRect(QRectF(0, 0, kWatermarkWidth, horizon), sky);

    QLinearGradient water(0, horizon, 0, kWatermarkHeight);
    water.setColorAt(0.0, kDeep.darker(140));
    water.setColorAt(1.0, kInk);
    p.fillRect(QRectF(0, horizon, kWatermarkWidth, kWatermarkHeight - horizon), water);

    // A warm haze where the two meet, so the horizon is a light source rather
    // than a hard edge between two flat fills.
    QRadialGradient haze(QPointF(kWatermarkWidth * 0.5, horizon), kWatermarkWidth * 0.9);
    haze.setColorAt(0.0, QColor(kGold.red(), kGold.green(), kGold.blue(), 70));
    haze.setColorAt(1.0, QColor(kGold.red(), kGold.green(), kGold.blue(), 0));
    p.fillRect(pm.rect(), haze);

    drawDome(p, QPointF(kWatermarkWidth * 0.52, horizon - 46), 34, 0.95);
    // Its reflection, dimmer and squashed, on the water below.
    p.save();
    p.translate(0, horizon);
    p.scale(1.0, -0.45);
    p.translate(0, -horizon);
    drawDome(p, QPointF(kWatermarkWidth * 0.52, horizon - 46), 34, 0.22);
    p.restore();

    const qreal basePoints = p.font().pointSizeF();

    p.setPen(QColor(0xe8, 0xe2, 0xd4));
    QFont title = p.font();
    title.setPointSizeF(basePoints * 2.2);
    title.setWeight(QFont::Light);
    p.setFont(title);
    p.drawText(QRectF(16, kWatermarkHeight - 74, kWatermarkWidth - 32, 34),
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Riven"));

    p.setPen(QColor(0x9d, 0xb4, 0xc0));
    QFont sub = p.font();
    sub.setPointSizeF(basePoints * 0.95);
    sub.setWeight(QFont::Normal);
    p.setFont(sub);
    p.drawText(QRectF(16, kWatermarkHeight - 42, kWatermarkWidth - 32, 22),
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("for Nintendo DS"));

    p.end();
    cached = pm;
    return cached;
}

QPixmap headerBanner()
{
    static QPixmap cached;
    if (!cached.isNull())
        return cached;

    QPixmap pm(kBannerWidth, kBannerHeight);
    QPainter p(&pm);

    // Top to bottom, since across is a single pixel: white at the top so the
    // title sits on plain paper, warming into the parchment above the rule.
    QLinearGradient wash(0, 0, 0, kBannerHeight);
    wash.setColorAt(0.0, QColor(0xff, 0xff, 0xff));
    wash.setColorAt(1.0, kPaper);
    p.fillRect(pm.rect(), wash);

    // A gold rule along the bottom, which is the only thing separating the
    // header from the page in ModernStyle once the default white is replaced.
    p.fillRect(QRect(0, kBannerHeight - 2, kBannerWidth, 2),
               QColor(kGold.red(), kGold.green(), kGold.blue(), 150));

    p.end();
    cached = pm;
    return cached;
}

int headerBannerWidth()
{
    return kBannerWidth;
}

QPixmap headerLogo()
{
    static QPixmap cached;
    if (!cached.isNull())
        return cached;

    QPixmap pm(kLogoSize, kLogoSize);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    drawDome(p, QPointF(kLogoSize / 2.0, kLogoSize / 2.0), kLogoSize * 0.36, 1.0);
    p.end();

    cached = pm;
    return cached;
}

} // namespace banner
