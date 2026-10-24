#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QStackedLayout;
class QVBoxLayout;

namespace zn {

// Start screen: create or open a vault, then the lock screen asking for its password.
class WelcomeWidget : public QWidget {
    Q_OBJECT
public:
    explicit WelcomeWidget(QWidget* parent = nullptr);

    void showStart();
    void showUnlock(const QString& path, const QString& error = {});
    void setBusy(bool busy, const QString& message = {});
    QString currentPath() const { return path_; }

    static QStringList recentVaults();
    static void rememberVault(const QString& path);
    static void forgetVault(const QString& path);

signals:
    void unlockRequested(const QString& path, const QString& password);
    void createRequested(const QString& path, const QString& password, bool withSamples);

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    void chooseOpen();
    void chooseCreate();
    void rebuildRecent();

    QStackedLayout* stack_;
    QVBoxLayout* recentLayout_;
    QLabel* vaultName_;
    QLabel* vaultPath_;
    QLabel* error_;
    QLineEdit* password_;
    QPushButton* unlock_;
    QString path_;
};

}  // namespace zn
