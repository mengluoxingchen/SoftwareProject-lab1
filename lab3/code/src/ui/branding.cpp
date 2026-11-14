#include "ui/branding.h"

#include <QFont>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QWidget>

namespace ledger {
namespace {

void paintMark(QPainter& painter, const QRectF& bounds) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(bounds.topLeft());
    painter.scale(bounds.width() / 64.0, bounds.height() / 64.0);
    QLinearGradient background(4, 0, 60, 64);
    background.setColorAt(0, QColor(QStringLiteral("#a78bfa")));
    background.setColorAt(1, QColor(QStringLiteral("#7435ca")));
    painter.setPen(Qt::NoPen);
    painter.setBrush(background);
    painter.drawRoundedRect(QRectF(1, 1, 62, 62), 16, 16);

    painter.setBrush(QColor(255, 255, 255, 100));
    painter.drawRoundedRect(QRectF(13, 12, 29, 37), 5, 5);
    painter.setBrush(QColor(QStringLiteral("#fbf9ff")));
    painter.drawRoundedRect(QRectF(21, 17, 29, 37), 5, 5);
    painter.setPen(QPen(QColor(QStringLiteral("#8954c7")), 2.4, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(QPointF(28, 27), QPointF(42, 27));
    painter.drawLine(QPointF(28, 34), QPointF(38, 34));
    painter.setPen(QPen(QColor(QStringLiteral("#c4a8f2")), 2));
    painter.setBrush(QColor(QStringLiteral("#4c1689")));
    painter.drawEllipse(QPointF(46, 47), 11, 11);
    QPainterPath check;
    check.moveTo(40, 47);
    check.lineTo(44, 51);
    check.lineTo(52, 42);
    painter.setPen(QPen(Qt::white, 2.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(check);
    painter.restore();
}

class LogoMark final : public QWidget {
public:
    LogoMark(int size, QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("brandLogo"));
        setFixedSize(size, size);
        setAccessibleName(QStringLiteral("共享记账标志"));
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        paintMark(painter, rect());
    }
};

}  // namespace

QIcon sharedLedgerIcon() {
    QIcon icon;
    for (const int size : {16, 24, 32, 48, 64, 128, 256}) {
        QPixmap image(size, size);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        paintMark(painter, QRectF(0, 0, size, size));
        painter.end();
        icon.addPixmap(image);
    }
    return icon;
}

QWidget* createBrandWidget(bool compact, QWidget* parent) {
    auto* brand = new QWidget(parent);
    brand->setObjectName(QStringLiteral("brandLockup"));
    auto* layout = new QHBoxLayout(brand);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(compact ? 11 : 14);
    layout->addWidget(new LogoMark(compact ? 38 : 48, brand));
    auto* wordmark = new QLabel(QStringLiteral("共享记账"), brand);
    wordmark->setObjectName(compact ? QStringLiteral("brandCompact") : QStringLiteral("brand"));
    QFont font(QStringLiteral("DengXian"));
    if (!QFontDatabase::families().contains(font.family())) {
        font.setFamily(QStringLiteral("Microsoft YaHei UI"));
    }
    font.setPixelSize(compact ? 22 : 27);
    font.setWeight(QFont::DemiBold);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 1.1);
    wordmark->setFont(font);
    layout->addWidget(wordmark);
    layout->addStretch();
    return brand;
}

}  // namespace ledger
