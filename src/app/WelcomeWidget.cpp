#include "WelcomeWidget.h"

#include "Icons.h"
#include "Theme.h"
#include "Vault.h"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QStackedLayout>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <cmath>

namespace zn {

namespace {

constexpr char kRecentKey[] = "recentVaults";

QLabel* bigIcon(const QString& name, QWidget* parent, int size = 64) {
    auto* l = new QLabel(parent);
    l->setPixmap(icons::pixmap(name, size, Theme::palette().accent, parent->devicePixelRatioF()));
    l->setAlignment(Qt::AlignCenter);
    return l;
}

// Rough entropy estimate, only to warn about obviously weak passwords.
int passwordScore(const QString& pw) {
    int pool = 0;
    bool lower = false, upper = false, digit = false, other = false;
    for (QChar c : pw) {
        if (c.isLower())
            lower = true;
        else if (c.isUpper())
            upper = true;
        else if (c.isDigit())
            digit = true;
        else
            other = true;
    }
    pool = (lower ? 26 : 0) + (upper ? 26 : 0) + (digit ? 10 : 0) + (other ? 33 : 0);
    const double bits = pw.size() * std::log2(std::max(pool, 2));
    return bits < 40 ? 0 : bits < 60 ? 1 : bits < 80 ? 2 : 3;
}

class CreateVaultDialog : public QDialog {
public:
    CreateVaultDialog(const QString& path, QWidget* parent) : QDialog(parent) {
        setWindowTitle(tr("New Vault"));
        auto* lay = new QVBoxLayout(this);
        auto* info = new QLabel(tr("<b>%1</b><br>Notes, titles and folder names are encrypted with this password "
                                   "(Argon2id + XChaCha20-Poly1305). <b>There is no way to recover a forgotten password.</b>")
                                    .arg(QFileInfo(path).fileName().toHtmlEscaped()),
                                this);
        info->setWordWrap(true);
        lay->addWidget(info);
        auto* form = new QFormLayout;
        pw_ = new QLineEdit(this);
        pw_->setEchoMode(QLineEdit::Password);
        confirm_ = new QLineEdit(this);
        confirm_->setEchoMode(QLineEdit::Password);
        strength_ = new QProgressBar(this);
        strength_->setRange(0, 3);
        strength_->setTextVisible(false);
        strength_->setFixedHeight(6);
        form->addRow(tr("Password:"), pw_);
        form->addRow(QString(), strength_);
        form->addRow(tr("Confirm:"), confirm_);
        lay->addLayout(form);
        samples_ = new QCheckBox(tr("Add sample notes"), this);
        samples_->setChecked(true);
        lay->addWidget(samples_);
        hint_ = new QLabel(this);
        hint_->setStyleSheet(QStringLiteral("color: #d73a3a"));
        lay->addWidget(hint_);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
        ok_ = buttons->addButton(tr("Create Vault"), QDialogButtonBox::AcceptRole);
        ok_->setProperty("accent", true);
        ok_->setDefault(true);
        lay->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        auto validate = [this] {
            const int score = passwordScore(pw_->text());
            strength_->setValue(pw_->text().isEmpty() ? 0 : std::min(3, score + 1));
            QString msg;
            if (!pw_->text().isEmpty() && score == 0)
                msg = tr("This password is easy to guess.");
            if (!confirm_->text().isEmpty() && confirm_->text() != pw_->text())
                msg = tr("Passwords do not match.");
            hint_->setText(msg);
            ok_->setEnabled(!pw_->text().isEmpty() && pw_->text() == confirm_->text());
        };
        connect(pw_, &QLineEdit::textChanged, this, validate);
        connect(confirm_, &QLineEdit::textChanged, this, validate);
        validate();
        resize(440, sizeHint().height());
    }
    QString password() const { return pw_->text(); }
    bool samples() const { return samples_->isChecked(); }

private:
    QLineEdit* pw_;
    QLineEdit* confirm_;
    QProgressBar* strength_;
    QCheckBox* samples_;
    QLabel* hint_;
    QPushButton* ok_;
};

}  // namespace

WelcomeWidget::WelcomeWidget(QWidget* parent) : QWidget(parent) {
    stack_ = new QStackedLayout(this);

    // ---- start page
    auto* start = new QWidget(this);
    auto* sl = new QVBoxLayout(start);
    sl->addStretch(2);
    sl->addWidget(bigIcon("vault", start, 72));
    auto* title = new QLabel(QStringLiteral("ZipNotes"), start);
    title->setAlignment(Qt::AlignCenter);
    title->setFont(Theme::instance().uiFont(QFont().pointSizeF() + 12, QFont::Bold));
    sl->addWidget(title);
    auto* sub = new QLabel(tr("Encrypted notes in a single .zip file"), start);
    sub->setAlignment(Qt::AlignCenter);
    sub->setStyleSheet(QStringLiteral("color: %1").arg(Theme::palette().secondaryText.name()));
    sl->addWidget(sub);
    sl->addSpacing(22);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    auto* create = new QPushButton(icons::icon("compose"), tr("New Vault…"), start);
    create->setProperty("accent", true);
    auto* open = new QPushButton(icons::icon("folder"), tr("Open Vault…"), start);
    buttons->addWidget(create);
    buttons->addWidget(open);
    buttons->addStretch();
    sl->addLayout(buttons);
    sl->addSpacing(24);
    recentLayout_ = new QVBoxLayout;
    recentLayout_->setSpacing(2);
    auto* recentBox = new QHBoxLayout;
    recentBox->addStretch();
    recentBox->addLayout(recentLayout_);
    recentBox->addStretch();
    sl->addLayout(recentBox);
    sl->addStretch(3);
    stack_->addWidget(start);

    // ---- lock page
    auto* lockPage = new QWidget(this);
    auto* ll = new QVBoxLayout(lockPage);
    ll->addStretch(2);
    ll->addWidget(bigIcon("lockfill", lockPage, 56));
    auto* lockedTitle = new QLabel(tr("This vault is locked."), lockPage);
    lockedTitle->setAlignment(Qt::AlignCenter);
    lockedTitle->setFont(Theme::instance().uiFont(QFont().pointSizeF() + 5, QFont::Bold));
    ll->addWidget(lockedTitle);
    vaultName_ = new QLabel(lockPage);
    vaultName_->setAlignment(Qt::AlignCenter);
    vaultName_->setFont(Theme::instance().uiFont(QFont().pointSizeF() + 1, QFont::DemiBold));
    ll->addWidget(vaultName_);
    vaultPath_ = new QLabel(lockPage);
    vaultPath_->setAlignment(Qt::AlignCenter);
    vaultPath_->setStyleSheet(QStringLiteral("color: %1").arg(Theme::palette().tertiaryText.name()));
    ll->addWidget(vaultPath_);
    ll->addSpacing(14);
    auto* pwRow = new QHBoxLayout;
    pwRow->addStretch();
    password_ = new QLineEdit(lockPage);
    password_->setEchoMode(QLineEdit::Password);
    password_->setPlaceholderText(tr("Enter the vault password"));
    password_->setMinimumWidth(280);
    password_->addAction(icons::icon("key"), QLineEdit::LeadingPosition);
    unlock_ = new QPushButton(tr("Unlock"), lockPage);
    unlock_->setProperty("accent", true);
    pwRow->addWidget(password_);
    pwRow->addWidget(unlock_);
    pwRow->addStretch();
    ll->addLayout(pwRow);
    error_ = new QLabel(lockPage);
    error_->setAlignment(Qt::AlignCenter);
    error_->setStyleSheet(QStringLiteral("color: #d73a3a"));
    ll->addWidget(error_);
    auto* other = new QPushButton(tr("Choose Another Vault…"), lockPage);
    other->setFlat(true);
    other->setStyleSheet(QStringLiteral("QPushButton { border: none; background: transparent; color: %1; }").arg(Theme::palette().link.name()));
    auto* otherRow = new QHBoxLayout;
    otherRow->addStretch();
    otherRow->addWidget(other);
    otherRow->addStretch();
    ll->addLayout(otherRow);
    ll->addStretch(3);
    stack_->addWidget(lockPage);

    connect(create, &QPushButton::clicked, this, &WelcomeWidget::chooseCreate);
    connect(open, &QPushButton::clicked, this, &WelcomeWidget::chooseOpen);
    connect(other, &QPushButton::clicked, this, &WelcomeWidget::showStart);
    auto submit = [this] {
        if (!password_->text().isEmpty())
            emit unlockRequested(path_, password_->text());
    };
    connect(unlock_, &QPushButton::clicked, this, submit);
    connect(password_, &QLineEdit::returnPressed, this, submit);
    showStart();
}

void WelcomeWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), Theme::palette().editor);
}

void WelcomeWidget::showStart() {
    rebuildRecent();
    stack_->setCurrentIndex(0);
}

void WelcomeWidget::showUnlock(const QString& path, const QString& error) {
    path_ = path;
    vaultName_->setText(QFileInfo(path).completeBaseName());
    vaultPath_->setText(QDir::toNativeSeparators(path));
    error_->setText(error);
    password_->clear();
    stack_->setCurrentIndex(1);
    password_->setFocus();
    setBusy(false);
}

void WelcomeWidget::setBusy(bool busy, const QString& message) {
    password_->setEnabled(!busy);
    unlock_->setEnabled(!busy);
    if (busy)
        error_->setText(QStringLiteral("<span style='color:%1'>%2</span>").arg(Theme::palette().secondaryText.name(), message));
    setCursor(busy ? Qt::BusyCursor : Qt::ArrowCursor);
}

QStringList WelcomeWidget::recentVaults() {
    QStringList list = QSettings().value(kRecentKey).toStringList();
    list.erase(std::remove_if(list.begin(), list.end(), [](const QString& p) { return !QFileInfo::exists(p); }), list.end());
    return list;
}

void WelcomeWidget::rememberVault(const QString& path) {
    QStringList list = QSettings().value(kRecentKey).toStringList();
    list.removeAll(path);
    list.prepend(path);
    QSettings().setValue(kRecentKey, list.mid(0, 8));
}

void WelcomeWidget::forgetVault(const QString& path) {
    QStringList list = QSettings().value(kRecentKey).toStringList();
    list.removeAll(path);
    QSettings().setValue(kRecentKey, list);
}

void WelcomeWidget::rebuildRecent() {
    while (QLayoutItem* it = recentLayout_->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    const QStringList recent = recentVaults();
    if (recent.isEmpty())
        return;
    auto* head = new QLabel(tr("Recent Vaults"), this);
    head->setFont(Theme::instance().uiFont(QFont().pointSizeF() - 0.5, QFont::Bold));
    head->setStyleSheet(QStringLiteral("color: %1").arg(Theme::palette().tertiaryText.name()));
    recentLayout_->addWidget(head);
    for (const QString& path : recent) {
        auto* b = new QPushButton(icons::icon("lock"), QStringLiteral("%1   %2").arg(QFileInfo(path).completeBaseName(),
                                                                            QDir::toNativeSeparators(QFileInfo(path).absolutePath())), this);
        b->setFlat(true);
        b->setMinimumWidth(380);
        b->setStyleSheet(QStringLiteral("QPushButton { text-align: left; border: none; padding: 6px 10px; background: transparent; }"
                                        "QPushButton:hover { background: %1; border-radius: 6px; }")
                             .arg(Theme::palette().hover.name(QColor::HexArgb)));
        connect(b, &QPushButton::clicked, this, [this, path] { showUnlock(path); });
        recentLayout_->addWidget(b);
    }
}

void WelcomeWidget::chooseOpen() {
    const QString start = recentVaults().value(0, QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
    const QString path = QFileDialog::getOpenFileName(this, tr("Open Vault"), QFileInfo(start).absolutePath(),
                                                      tr("ZipNotes vaults (*.zip);;All files (*)"));
    if (path.isEmpty())
        return;
    if (!Vault::looksLikeVault(path)) {
        QMessageBox::warning(this, tr("Open Vault"), tr("%1 is not a ZipNotes vault.").arg(QFileInfo(path).fileName()));
        return;
    }
    showUnlock(path);
}

void WelcomeWidget::chooseCreate() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QString path = QFileDialog::getSaveFileName(this, tr("New Vault"), QDir(dir).filePath(tr("Notes.zip")), tr("ZipNotes vaults (*.zip)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(QLatin1String(".zip"), Qt::CaseInsensitive))
        path += QStringLiteral(".zip");
    if (QFileInfo::exists(path)) {
        QMessageBox::warning(this, tr("New Vault"), tr("%1 already exists. Choose another name.").arg(QFileInfo(path).fileName()));
        return;
    }
    CreateVaultDialog dlg(path, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    path_ = path;
    emit createRequested(path, dlg.password(), dlg.samples());
}

}  // namespace zn
