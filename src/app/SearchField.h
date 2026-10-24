#pragma once

#include "SearchQuery.h"

#include <QFrame>
#include <QTimer>
#include <QWidget>

class QLineEdit;
class QHBoxLayout;

namespace zn {

class VaultController;

// A restriction shown as a chip in the search field.
struct SearchToken {
    enum Kind { Tag, Folder, Has, Pinned, Modified };
    Kind kind = Tag;
    QString value;  // tag, folder id, has-key, days
    QString label;
    QString icon;
    bool operator==(const SearchToken& o) const { return kind == o.kind && value == o.value; }
};

struct Suggestion {
    bool header = false;
    QString text;
    QString icon;
    SearchToken token;
};

// The floating list under the search field: "Suggested Searches" when the field is empty, matching tags
// and folders while typing.
class SuggestionPopup : public QFrame {
    Q_OBJECT
public:
    explicit SuggestionPopup(QWidget* parent);
    void setSuggestions(const QList<Suggestion>& s);
    bool isEmpty() const { return items_.isEmpty(); }
    void moveSelection(int delta);
    bool activateCurrent();

signals:
    void picked(const SearchToken& token);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;

private:
    int rowAt(int y) const;
    int rowTop(int row) const;
    int rowHeight(int row) const;
    QList<Suggestion> items_;
    int current_ = -1;
};

class SearchField : public QWidget {
    Q_OBJECT
public:
    explicit SearchField(VaultController& vc, QWidget* parent = nullptr);

    SearchQuery query() const;
    bool isEmpty() const;
    QString text() const;
    void clear();
    void focusField();
    void setPopupHost(QWidget* host) { host_ = host; }
    void addToken(const SearchToken& token);

signals:
    void queryChanged();
    void cancelled();
    void moveToResults();

protected:
    bool eventFilter(QObject* o, QEvent* e) override;
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;

private:
    void textEdited();
    void updateSuggestions();
    void placePopup();
    void hidePopup();
    void removeToken(int index);
    void rebuildChips();
    QString lastWord() const;

    VaultController& vc_;
    QLineEdit* edit_;
    QHBoxLayout* chipLayout_;
    QList<SearchToken> tokens_;
    QList<QWidget*> chips_;
    SuggestionPopup* popup_ = nullptr;
    QWidget* host_ = nullptr;
    QTimer debounce_;
};

}  // namespace zn
