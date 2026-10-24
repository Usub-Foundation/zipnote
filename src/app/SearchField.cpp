#include "SearchField.h"

#include "Icons.h"
#include "TextNormalizer.h"
#include "Theme.h"
#include "VaultController.h"

#include <QApplication>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <algorithm>
#include <functional>

namespace zn {

namespace {

constexpr int kRow = 30;
constexpr int kHeaderRow = 26;
constexpr int kDebounceMs = 60;  // coalesce fast typing; each search is cancellable anyway

struct HasSuggestion {
    const char* key;
    const char* label;
    const char* icon;
};

const HasSuggestion kHas[] = {
    {"checklist", QT_TRANSLATE_NOOP("SearchField", "Notes with Checklists"), "checklist"},
    {"todo", QT_TRANSLATE_NOOP("SearchField", "Unchecked Items"), "checklist"},
    {"diagram", QT_TRANSLATE_NOOP("SearchField", "Notes with Diagrams"), "diagram"},
    {"excalidraw", QT_TRANSLATE_NOOP("SearchField", "Drawings"), "drawing"},
    {"table", QT_TRANSLATE_NOOP("SearchField", "Notes with Tables"), "table"},
    {"link", QT_TRANSLATE_NOOP("SearchField", "Notes with Links"), "link"},
    {"tag", QT_TRANSLATE_NOOP("SearchField", "Notes with Tags"), "tag"},
};

class Chip : public QWidget {
public:
    Chip(const SearchToken& t, std::function<void()> remove, QWidget* parent)
        : QWidget(parent), token_(t), remove_(std::move(remove)) {
        setCursor(Qt::ArrowCursor);
        const QFontMetrics fm(Theme::instance().uiFont(QFont().pointSizeF()));
        setFixedSize(fm.horizontalAdvance(t.label) + 44, 22);
        setToolTip(tr("Remove"));
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const ThemePalette& pal = Theme::palette();
        QPainterPath path;
        path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
        p.fillPath(path, pal.chip);
        p.drawPixmap(QRect(6, 4, 14, 14), icons::pixmap(token_.icon, 14, pal.chipText, devicePixelRatioF()));
        p.setFont(Theme::instance().uiFont(QFont().pointSizeF(), QFont::DemiBold));
        p.setPen(pal.chipText);
        p.drawText(QRect(24, 0, width() - 42, height()), Qt::AlignLeft | Qt::AlignVCenter, token_.label);
        p.drawPixmap(QRect(width() - 17, 5, 12, 12), icons::pixmap("close", 12, pal.chipText, devicePixelRatioF()));
    }
    void mousePressEvent(QMouseEvent*) override { remove_(); }

private:
    SearchToken token_;
    std::function<void()> remove_;
};

Suggestion header(const QString& text) {
    Suggestion s;
    s.header = true;
    s.text = text;
    return s;
}

}  // namespace

// ------------------------------------------------------------------------------------------------ popup

SuggestionPopup::SuggestionPopup(QWidget* parent) : QFrame(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_ShowWithoutActivating);
    auto* shadow = new QGraphicsDropShadowEffect(this);
    shadow->setBlurRadius(24);
    shadow->setOffset(0, 6);
    shadow->setColor(QColor(0, 0, 0, 70));
    setGraphicsEffect(shadow);
    hide();
}

void SuggestionPopup::setSuggestions(const QList<Suggestion>& s) {
    items_ = s;
    current_ = -1;
    int h = 8;
    for (int i = 0; i < items_.size(); ++i)
        h += rowHeight(i);
    resize(width(), h + 4);
    update();
}

int SuggestionPopup::rowHeight(int row) const {
    return items_[row].header ? kHeaderRow : kRow;
}

int SuggestionPopup::rowTop(int row) const {
    int y = 6;
    for (int i = 0; i < row; ++i)
        y += rowHeight(i);
    return y;
}

int SuggestionPopup::rowAt(int y) const {
    for (int i = 0; i < items_.size(); ++i)
        if (y >= rowTop(i) && y < rowTop(i) + rowHeight(i))
            return items_[i].header ? -1 : i;
    return -1;
}

void SuggestionPopup::moveSelection(int delta) {
    if (items_.isEmpty())
        return;
    int i = current_;
    for (int guard = 0; guard < items_.size(); ++guard) {
        i = (i + delta + static_cast<int>(items_.size())) % static_cast<int>(items_.size());
        if (!items_[i].header) {
            current_ = i;
            break;
        }
    }
    update();
}

bool SuggestionPopup::activateCurrent() {
    if (current_ < 0 || current_ >= items_.size() || items_[current_].header)
        return false;
    emit picked(items_[current_].token);
    return true;
}

void SuggestionPopup::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const ThemePalette& pal = Theme::palette();
    QPainterPath bg;
    bg.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);
    p.fillPath(bg, pal.dark ? QColor(0x2C, 0x2C, 0x2E) : QColor(0xFF, 0xFF, 0xFF));
    p.setPen(pal.separator);
    p.drawPath(bg);
    for (int i = 0; i < items_.size(); ++i) {
        const Suggestion& s = items_[i];
        const QRect r(0, rowTop(i), width(), rowHeight(i));
        if (s.header) {
            p.setFont(Theme::instance().uiFont(QFont().pointSizeF() - 0.5, QFont::Bold));
            p.setPen(pal.tertiaryText);
            p.drawText(r.adjusted(14, 4, -10, 0), Qt::AlignLeft | Qt::AlignVCenter, s.text);
            continue;
        }
        if (i == current_) {
            QPainterPath path;
            path.addRoundedRect(QRectF(r).adjusted(6, 1, -6, -1), 6, 6);
            p.fillPath(path, pal.accent);
        }
        const QColor fg = i == current_ ? QColor(0x1D, 0x1D, 0x1F) : pal.title;
        p.drawPixmap(QRect(16, r.top() + (r.height() - 16) / 2, 16, 16),
                     icons::pixmap(s.icon, 16, i == current_ ? fg : pal.accent, devicePixelRatioF()));
        p.setFont(Theme::instance().uiFont(QFont().pointSizeF() + 0.5));
        p.setPen(fg);
        p.drawText(r.adjusted(42, 0, -12, 0), Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(p.font()).elidedText(s.text, Qt::ElideRight, r.width() - 54));
    }
}

void SuggestionPopup::mouseMoveEvent(QMouseEvent* e) {
    const int r = rowAt(e->pos().y());
    if (r != current_ && r >= 0) {
        current_ = r;
        update();
    }
}

void SuggestionPopup::mousePressEvent(QMouseEvent* e) {
    const int r = rowAt(e->pos().y());
    if (r >= 0) {
        current_ = r;
        activateCurrent();
    }
}

void SuggestionPopup::leaveEvent(QEvent*) {
    current_ = -1;
    update();
}

// ------------------------------------------------------------------------------------------------ field

SearchField::SearchField(VaultController& vc, QWidget* parent) : QWidget(parent), vc_(vc) {
    setFixedHeight(30);
    setMinimumWidth(220);
    setCursor(Qt::IBeamCursor);
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(30, 3, 6, 3);
    lay->setSpacing(4);
    chipLayout_ = new QHBoxLayout;
    chipLayout_->setSpacing(4);
    lay->addLayout(chipLayout_);
    edit_ = new QLineEdit(this);
    edit_->setFrame(false);
    edit_->setPlaceholderText(tr("Search"));
    edit_->setStyleSheet(QStringLiteral("QLineEdit { background: transparent; border: none; padding: 0; }"));
    edit_->installEventFilter(this);
    lay->addWidget(edit_, 1);
    auto* clearButton = new QToolButton(this);
    clearButton->setIcon(icons::icon("clear", Theme::palette().tertiaryText));
    clearButton->setAutoRaise(true);
    clearButton->setCursor(Qt::ArrowCursor);
    clearButton->setFixedSize(20, 20);
    clearButton->setToolTip(tr("Clear Search"));
    clearButton->hide();
    lay->addWidget(clearButton);

    debounce_.setSingleShot(true);
    debounce_.setInterval(kDebounceMs);
    connect(&debounce_, &QTimer::timeout, this, &SearchField::queryChanged);
    connect(edit_, &QLineEdit::textEdited, this, &SearchField::textEdited);
    connect(clearButton, &QToolButton::clicked, this, [this] {
        clear();
        emit cancelled();
    });
    connect(this, &SearchField::queryChanged, clearButton, [this, clearButton] { clearButton->setVisible(!isEmpty()); });
}

QString SearchField::text() const {
    return edit_->text();
}

bool SearchField::isEmpty() const {
    return edit_->text().trimmed().isEmpty() && tokens_.isEmpty();
}

SearchQuery SearchField::query() const {
    SearchQuery q = SearchQuery::parse(edit_->text());
    for (const auto& t : tokens_) {
        switch (t.kind) {
        case SearchToken::Tag: q.tags.append(text::fold(t.value)); break;
        case SearchToken::Folder: q.folderIds += vc_.folderWithDescendants(t.value); break;
        case SearchToken::Has: q.has.insert(t.value); break;
        case SearchToken::Pinned: q.pinnedOnly = true; break;
        case SearchToken::Modified: q.modifiedAfter = QDate::currentDate().addDays(-t.value.toInt()); break;
        }
    }
    return q;
}

void SearchField::clear() {
    edit_->clear();
    tokens_.clear();
    rebuildChips();
    hidePopup();
    debounce_.stop();
    emit queryChanged();
}

void SearchField::focusField() {
    edit_->setFocus(Qt::ShortcutFocusReason);
    edit_->selectAll();
    updateSuggestions();
}

void SearchField::addToken(const SearchToken& token) {
    if (!tokens_.contains(token))
        tokens_.append(token);
    rebuildChips();
    emit queryChanged();
}

void SearchField::removeToken(int index) {
    if (index < 0 || index >= tokens_.size())
        return;
    tokens_.removeAt(index);
    rebuildChips();
    edit_->setFocus();
    emit queryChanged();
}

void SearchField::rebuildChips() {
    for (QWidget* c : chips_)
        c->deleteLater();
    chips_.clear();
    for (int i = 0; i < tokens_.size(); ++i) {
        auto* chip = new Chip(tokens_[i], [this, i] { QMetaObject::invokeMethod(this, [this, i] { removeToken(i); }, Qt::QueuedConnection); }, this);
        chipLayout_->addWidget(chip);
        chips_.append(chip);
    }
    edit_->setPlaceholderText(tokens_.isEmpty() ? tr("Search") : QString());
}

QString SearchField::lastWord() const {
    const QString t = edit_->text();
    const int space = static_cast<int>(t.lastIndexOf(u' '));
    QString w = t.mid(space + 1);
    if (w.startsWith(u'#'))
        w = w.mid(1);
    return w;
}

void SearchField::textEdited() {
    debounce_.start();
    updateSuggestions();
}

void SearchField::updateSuggestions() {
    if (!vc_.isOpen() || !edit_->hasFocus()) {
        hidePopup();
        return;
    }
    if (!popup_) {
        popup_ = new SuggestionPopup(host_ ? host_ : window());
        connect(popup_, &SuggestionPopup::picked, this, [this](const SearchToken& t) {
            // the typed word becomes the token
            QString text = edit_->text();
            const int space = static_cast<int>(text.lastIndexOf(u' '));
            if (!lastWord().isEmpty())
                text = text.left(space + 1);
            edit_->setText(text.trimmed().isEmpty() ? QString() : text);
            addToken(t);
            hidePopup();
            edit_->setFocus();
        });
    }

    const QString word = lastWord();
    const QString needle = text::fold(word);
    const bool typingHash = edit_->text().section(u' ', -1).startsWith(u'#');
    QList<Suggestion> list;

    QList<Suggestion> suggested;
    for (const auto& h : kHas) {
        const QString label = tr(h.label);
        if (needle.isEmpty() || text::containsFolded(label, needle) || QString::fromLatin1(h.key).startsWith(needle))
            suggested.append({false, label, QString::fromLatin1(h.icon), {SearchToken::Has, QString::fromLatin1(h.key), label, QString::fromLatin1(h.icon)}});
    }
    if (needle.isEmpty() || text::containsFolded(tr("Pinned Notes"), needle))
        suggested.append({false, tr("Pinned Notes"), QStringLiteral("pin"), {SearchToken::Pinned, QStringLiteral("1"), tr("Pinned"), QStringLiteral("pin")}});
    if (needle.isEmpty() || text::containsFolded(tr("Edited in the Last 7 Days"), needle))
        suggested.append({false, tr("Edited in the Last 7 Days"), QStringLiteral("recent"), {SearchToken::Modified, QStringLiteral("7"), tr("Last 7 Days"), QStringLiteral("recent")}});

    QList<Suggestion> tags;
    for (const auto& t : vc_.tags(needle)) {
        tags.append({false, u'#' + t.tag, QStringLiteral("tag"), {SearchToken::Tag, t.tag, u'#' + t.tag, QStringLiteral("tag")}});
        if (tags.size() >= 6)
            break;
    }
    QList<Suggestion> folders;
    if (!needle.isEmpty() && !typingHash) {
        for (const auto& f : vc_.folders()) {
            if (!text::containsFolded(f.name, needle))  // substring, ignoring case and accents
                continue;
            folders.append({false, vc_.folderPath(f.id), QStringLiteral("folder"), {SearchToken::Folder, f.id, f.name, QStringLiteral("folder")}});
            if (folders.size() >= 5)
                break;
        }
    }

    if (typingHash) {
        if (!tags.isEmpty())
            list.append(header(tr("Tags")));
        list += tags;
    } else if (needle.isEmpty()) {
        list.append(header(tr("Suggested Searches")));
        list += suggested;
        if (!tags.isEmpty()) {
            list.append(header(tr("Tags")));
            list += tags.mid(0, 4);
        }
    } else {
        if (!tags.isEmpty()) {
            list.append(header(tr("Tags")));
            list += tags;
        }
        if (!folders.isEmpty()) {
            list.append(header(tr("Folders")));
            list += folders;
        }
        if (!suggested.isEmpty()) {
            list.append(header(tr("Suggested Searches")));
            list += suggested;
        }
    }
    // tokens already in the field are not suggested again
    QList<Suggestion> filtered;
    for (const auto& s : list)
        if (s.header || !tokens_.contains(s.token))
            filtered.append(s);
    for (int i = static_cast<int>(filtered.size()) - 1; i >= 0; --i)  // drop headers left without items
        if (filtered[i].header && (i + 1 == filtered.size() || filtered[i + 1].header))
            filtered.removeAt(i);

    popup_->setSuggestions(filtered);
    if (popup_->isEmpty()) {
        hidePopup();
        return;
    }
    placePopup();
    popup_->show();
    popup_->raise();
}

void SearchField::placePopup() {
    if (!popup_)
        return;
    QWidget* host = popup_->parentWidget();
    const int w = std::max(width(), 300);
    QPoint pos = mapTo(host, QPoint(width() - w, height() + 6));
    pos.setX(std::clamp(pos.x(), 8, std::max(8, host->width() - w - 8)));
    popup_->setGeometry(pos.x(), pos.y(), w, popup_->height());
}

void SearchField::hidePopup() {
    if (popup_)
        popup_->hide();
}

bool SearchField::eventFilter(QObject* o, QEvent* e) {
    if (o != edit_)
        return QWidget::eventFilter(o, e);
    if (e->type() == QEvent::FocusIn) {
        updateSuggestions();
        update();
    } else if (e->type() == QEvent::FocusOut) {
        hidePopup();
        update();
    } else if (e->type() == QEvent::KeyPress) {
        auto* k = static_cast<QKeyEvent*>(e);
        const bool popupOpen = popup_ && popup_->isVisible();
        switch (k->key()) {
        case Qt::Key_Down:
            if (popupOpen) {
                popup_->moveSelection(1);
                return true;
            }
            emit moveToResults();
            return true;
        case Qt::Key_Up:
            if (popupOpen) {
                popup_->moveSelection(-1);
                return true;
            }
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (popupOpen && popup_->activateCurrent())
                return true;
            hidePopup();
            debounce_.stop();
            emit queryChanged();
            emit moveToResults();
            return true;
        case Qt::Key_Escape:
            if (popupOpen) {
                hidePopup();
                return true;
            }
            clear();
            emit cancelled();
            return true;
        case Qt::Key_Backspace:
            if (edit_->cursorPosition() == 0 && !edit_->hasSelectedText() && !tokens_.isEmpty()) {
                removeToken(static_cast<int>(tokens_.size()) - 1);
                return true;
            }
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(o, e);
}

void SearchField::mousePressEvent(QMouseEvent*) {
    edit_->setFocus();
}

void SearchField::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const ThemePalette& pal = Theme::palette();
    QPainterPath path;
    path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
    p.fillPath(path, pal.field);
    if (edit_->hasFocus()) {
        p.setPen(QPen(QColor(pal.accent.red(), pal.accent.green(), pal.accent.blue(), 170), 2));
        p.drawPath(path);
    }
    p.drawPixmap(QRect(9, (height() - 16) / 2, 16, 16), icons::pixmap("search", 16, pal.secondaryText, devicePixelRatioF()));
}

}  // namespace zn
