#include "FindBar.h"

#include "Icons.h"
#include "NoteEditor.h"
#include "TextNormalizer.h"
#include "Theme.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QTextBlock>
#include <QToolButton>

namespace zn {

FindBar::FindBar(NoteEditor* editor, QWidget* parent) : QWidget(parent), editor_(editor) {
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(14, 6, 10, 6);
    lay->setSpacing(6);
    field_ = new QLineEdit(this);
    field_->setPlaceholderText(tr("Find in Note"));
    field_->setClearButtonEnabled(true);
    field_->addAction(icons::icon("search"), QLineEdit::LeadingPosition);
    field_->installEventFilter(this);
    count_ = new QLabel(this);
    count_->setMinimumWidth(70);
    prev_ = new QToolButton(this);
    prev_->setIcon(icons::icon("chevronup"));
    prev_->setAutoRaise(true);
    prev_->setToolTip(tr("Previous (Shift+Enter)"));
    next_ = new QToolButton(this);
    next_->setIcon(icons::icon("chevrondown"));
    next_->setAutoRaise(true);
    next_->setToolTip(tr("Next (Enter)"));
    auto* done = new QPushButton(tr("Done"), this);
    lay->addWidget(field_, 1);
    lay->addWidget(count_);
    lay->addWidget(prev_);
    lay->addWidget(next_);
    lay->addWidget(done);
    connect(field_, &QLineEdit::textChanged, this, &FindBar::search);
    connect(prev_, &QToolButton::clicked, this, [this] { step(-1); });
    connect(next_, &QToolButton::clicked, this, [this] { step(1); });
    connect(done, &QPushButton::clicked, this, &FindBar::deactivate);
    hide();
}

void FindBar::activate(const QString& initial) {
    show();
    if (!initial.isEmpty())
        field_->setText(initial);
    field_->setFocus();
    field_->selectAll();
    search();
}

void FindBar::deactivate() {
    // only hand focus back if it was here: opening a search result closes the bar while the user is still
    // typing in the search field, and must not pull the caret into the note
    const bool hadFocus = isAncestorOf(QApplication::focusWidget());
    hide();
    matches_.clear();
    current_ = -1;
    editor_->setFindSelections({}, -1);
    if (hadFocus)
        editor_->setFocus();
}

void FindBar::refresh() {
    if (isVisible())
        search();
}

void FindBar::search() {
    matches_.clear();
    current_ = -1;
    const QString needle = text::fold(field_->text());
    if (!needle.isEmpty()) {
        QTextDocument* doc = editor_->document();
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
            const QString src = b.text();
            const text::Normalized n = text::normalize(src);
            for (int from = 0;;) {
                const int at = static_cast<int>(n.text.indexOf(needle, from));
                if (at < 0)
                    break;
                const auto [s, e] = n.sourceRange(at, at + static_cast<int>(needle.size()), static_cast<int>(src.size()));
                QTextCursor c(doc);
                c.setPosition(b.position() + s);
                c.setPosition(b.position() + e, QTextCursor::KeepAnchor);
                matches_.append(c);
                from = at + static_cast<int>(needle.size());
            }
        }
        // first match at or after the caret
        const int caret = editor_->textCursor().selectionStart();
        current_ = 0;
        for (int i = 0; i < matches_.size(); ++i)
            if (matches_[i].selectionStart() >= caret) {
                current_ = i;
                break;
            }
        if (matches_.isEmpty())
            current_ = -1;
    }
    step(0);
}

void FindBar::step(int delta) {
    if (!matches_.isEmpty()) {
        current_ = (current_ + delta + static_cast<int>(matches_.size())) % static_cast<int>(matches_.size());
        QTextCursor c = matches_[current_];
        c.clearSelection();
        editor_->setTextCursor(c);
        editor_->ensureCursorVisible();
    }
    editor_->setFindSelections(matches_, current_);
    count_->setText(field_->text().isEmpty() ? QString()
                    : matches_.isEmpty()    ? tr("Not found")
                                            : tr("%1 of %2").arg(current_ + 1).arg(matches_.size()));
    prev_->setEnabled(matches_.size() > 1);
    next_->setEnabled(matches_.size() > 1);
}

bool FindBar::eventFilter(QObject* o, QEvent* e) {
    if (o == field_ && e->type() == QEvent::KeyPress) {
        auto* k = static_cast<QKeyEvent*>(e);
        if (k->key() == Qt::Key_Escape) {
            deactivate();
            return true;
        }
        if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) {
            step(k->modifiers() & Qt::ShiftModifier ? -1 : 1);
            return true;
        }
    }
    return QWidget::eventFilter(o, e);
}

void FindBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), Theme::palette().sidebar);
    p.fillRect(QRect(0, height() - 1, width(), 1), Theme::palette().separator);
}

}  // namespace zn
