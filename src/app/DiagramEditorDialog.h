#pragma once

#include "NoteText.h"

#include <QDialog>
#include <QTimer>

class QLabel;
class QPlainTextEdit;
class QComboBox;

namespace zn {

class DiagramService;

// Source on the left, live preview on the right, for Mermaid and PlantUML blocks.
class DiagramEditorDialog : public QDialog {
    Q_OBJECT
public:
    DiagramEditorDialog(DiagramService& service, DiagramKind kind, const QString& source, QWidget* parent = nullptr);
    QString source() const;

    static QString defaultSource(DiagramKind kind);

private:
    void schedulePreview();
    void renderPreview();

    DiagramService& service_;
    DiagramKind kind_;
    QPlainTextEdit* editor_;
    QLabel* preview_;
    QLabel* status_;
    QComboBox* templates_;
    QTimer debounce_;
    int generation_ = 0;
};

}  // namespace zn
