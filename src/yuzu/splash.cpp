#include "splash.h"

#include <QApplication>
#include <QGraphicsOpacityEffect>
#include <QLabel>
#include <QPropertyAnimation>
#include <QScreen>
#include <QVBoxLayout>

SplashScreen::SplashScreen() : QWidget(nullptr) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setStyleSheet("background-color: transparent;");
    setFixedSize(520, 360);

    auto* container = new QWidget(this);
    container->setGeometry(0, 0, 520, 360);
    container->setStyleSheet("background-color: #0a0a0a; border-radius: 8px;");

    auto* layout = new QVBoxLayout(container);
    layout->setAlignment(Qt::AlignCenter);
    layout->setSpacing(12);
    layout->setContentsMargins(40, 30, 40, 30);

    nullworks_label = new QLabel("N U L L W O R K S", container);
    nullworks_label->setAlignment(Qt::AlignCenter);
    nullworks_label->setStyleSheet(
        "QLabel {"
        "  color: #d0d0d0;"
        "  font-family: 'Segoe UI', 'Arial', sans-serif;"
        "  font-size: 32px;"
        "  font-weight: 600;"
        "  background: transparent;"
        "}");
    layout->addWidget(nullworks_label);

    subtitle_label = new QLabel("loading...", container);
    subtitle_label->setAlignment(Qt::AlignCenter);
    subtitle_label->setStyleSheet(
        "QLabel {"
        "  color: #606060;"
        "  font-family: 'Segoe UI', 'Arial', sans-serif;"
        "  font-size: 11px;"
        "  background: transparent;"
        "}");
    layout->addWidget(subtitle_label);

    auto* progress_bar = new QWidget(container);
    progress_bar->setFixedHeight(2);
    progress_bar->setFixedWidth(440);
    progress_bar->setStyleSheet("background-color: #1a1a1a;");
    layout->addWidget(progress_bar, 0, Qt::AlignCenter);

    auto* fill = new QWidget(progress_bar);
    fill->setGeometry(0, 0, 0, 2);
    fill->setStyleSheet("background-color: #8a8a8a;");

    auto* progress_timer = new QTimer(this);
    connect(progress_timer, &QTimer::timeout, this, [fill]() {
        int w = fill->width();
        if (w < 440) {
            fill->setFixedWidth(w + 2);
        }
    });
    progress_timer->start(6);

    fade_effect = new QGraphicsOpacityEffect(this);
    container->setGraphicsEffect(fade_effect);

    anim_timer = new QTimer(this);
    connect(anim_timer, &QTimer::timeout, this, &SplashScreen::AnimateLetterSpacing);
    connect(this, &SplashScreen::Closed, this, &SplashScreen::close);
}

SplashScreen::~SplashScreen() = default;

void SplashScreen::Start() {
    const auto screens = QGuiApplication::screens();
    if (screens.isEmpty()) {
        return;
    }
    const auto screen = screens.first();
    const auto geom = screen->availableGeometry();
    move(geom.center().x() - 260, geom.center().y() - 180);

    fade_effect->setOpacity(0.0);
    show();

    auto* fade_in = new QPropertyAnimation(fade_effect, "opacity");
    fade_in->setDuration(600);
    fade_in->setStartValue(0.0);
    fade_in->setEndValue(1.0);
    fade_in->setEasingCurve(QEasingCurve::InOutCubic);
    fade_in->start(QAbstractAnimation::DeleteWhenStopped);

    anim_timer->start(60);
}

void SplashScreen::CloseSplash() {
    if (closing) {
        return;
    }
    closing = true;
    anim_timer->stop();

    subtitle_label->setText("");

    auto* fade_out = new QPropertyAnimation(fade_effect, "opacity");
    fade_out->setDuration(400);
    fade_out->setStartValue(1.0);
    fade_out->setEndValue(0.0);
    fade_out->setEasingCurve(QEasingCurve::InOutCubic);
    connect(fade_out, &QPropertyAnimation::finished, this, &SplashScreen::OnFadeOutFinished);
    fade_out->start(QAbstractAnimation::DeleteWhenStopped);
}

void SplashScreen::OnFadeOutFinished() {
    emit Closed();
}

void SplashScreen::AnimateLetterSpacing() {
    letter_spacing_step++;
    if (letter_spacing_step == 1) {
        nullworks_label->setText("N U L L W O R K S");
    } else if (letter_spacing_step == 6) {
        nullworks_label->setText("N   U   L   L   W   O   R   K   S");
    } else if (letter_spacing_step == 10) {
        nullworks_label->setText("N    U    L    L    W    O    R    K    S");
    } else if (letter_spacing_step == 16) {
        subtitle_label->setText("initializing...");
    } else if (letter_spacing_step == 28) {
        subtitle_label->setText("ready");
        anim_timer->stop();
    }
}
