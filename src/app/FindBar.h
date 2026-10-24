#pragma once

#include <QTextCursor>
#include <QWidget>

class QLabel;
class QLineEdit;
class QToolButton;

namespace zn {

class NoteEditor;

// "Find in Note" (Ctrl+F in the editor): substring search inside the open note, case- and
// diacritic-insensitive like the global search.
class FindBar : public QWidget {
    Q_OBJECT
public:
    explicit FindBar(NoteEditor* editor, QWidget* parent = nullptr);

    void activate(const QString& initial = {});
    void deactivate();
    void refresh();

protected:
    void paintEvent(QPaintEvent* e) override;
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    void search();
    void step(int delta);

    NoteEditor* editor_;
    QLineEdit* field_;
    QLabel* count_;
    QToolButton* prev_;
    QToolButton* next_;
    QList<QTextCursor> matches_;
    int current_ = -1;
};

}  // namespace zn
