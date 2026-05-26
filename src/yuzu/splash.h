#pragma once

#include <QTimer>
#include <QWidget>

class QLabel;
class QProgressBar;
class QGraphicsOpacityEffect;

class SplashScreen : public QWidget {
    Q_OBJECT

public:
    explicit SplashScreen();
    ~SplashScreen() override;

    void Start();
    void CloseSplash();

signals:
    void Closed();

private:
    void OnFadeOutFinished();
    void AnimateLetterSpacing();

    QLabel* nullworks_label;
    QLabel* subtitle_label;
    QTimer* anim_timer;
    QGraphicsOpacityEffect* fade_effect;
    int letter_spacing_step{0};
    bool closing{false};
};
