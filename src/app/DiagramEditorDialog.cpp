#include "DiagramEditorDialog.h"

#include "DiagramService.h"
#include "Icons.h"
#include "Theme.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QVBoxLayout>

namespace zn {

namespace {

struct Template {
    const char* name;
    const char* source;
};

const Template kMermaid[] = {
    {"Flowchart", "flowchart TD\n    A[Start] --> B{Is it working?}\n    B -->|Yes| C[Ship it]\n    B -->|No| D[Debug]\n    D --> B"},
    {"Sequence", "sequenceDiagram\n    participant App\n    participant Vault\n    App->>Vault: unlock(password)\n    Vault-->>App: notes"},
    {"Class", "classDiagram\n    class Vault {\n      +open(path, password)\n      +commit()\n    }\n    class Note\n    Vault \"1\" --> \"*\" Note"},
    {"State", "stateDiagram-v2\n    [*] --> Locked\n    Locked --> Unlocked: password\n    Unlocked --> Locked: idle 10 min"},
    {"Entity relationship", "erDiagram\n    FOLDER ||--o{ NOTE : contains\n    NOTE ||--o{ ATTACHMENT : has"},
    {"Gantt", "gantt\n    title Release\n    dateFormat YYYY-MM-DD\n    section Build\n    Core      :a1, 2026-11-01, 7d\n    UI        :after a1, 10d"},
    {"Pie", "pie title Time spent\n    \"Writing\" : 45\n    \"Reading\" : 30\n    \"Searching\" : 25"},
    {"Mind map", "mindmap\n  root((ZipNotes))\n    Encryption\n      Argon2id\n      XChaCha20\n    Search\n      Prefix\n      Typos\n    Diagrams"},
};

const Template kPlantUml[] = {
    {"Sequence", "@startuml\nactor User\nUser -> App: search \"meeting\"\nApp -> Index: query\nIndex --> App: top hits\n@enduml"},
    {"Use case", "@startuml\nleft to right direction\nactor User\nUser --> (Write note)\nUser --> (Search)\nUser --> (Lock vault)\n@enduml"},
    {"Class", "@startuml\nclass Vault {\n  +open()\n  +commit()\n}\nclass Note\nVault \"1\" *-- \"many\" Note\n@enduml"},
    {"Activity", "@startuml\nstart\n:Type a query;\nif (exact matches?) then (yes)\n  :Show results;\nelse (no)\n  :Typo-tolerant pass;\nendif\nstop\n@enduml"},
    {"Component", "@startuml\n[Editor] --> [Vault]\n[Editor] --> [Search Index]\n[Vault] --> [libzip]\n[Vault] --> [libsodium]\n@enduml"},
};

}  // namespace

QString DiagramEditorDialog::defaultSource(DiagramKind kind) {
    return QString::fromUtf8(kind == DiagramKind::PlantUml ? kPlantUml[0].source : kMermaid[0].source);
}

DiagramEditorDialog::DiagramEditorDialog(DiagramService& service, DiagramKind kind, const QString& source, QWidget* parent)
    : QDialog(parent), service_(service), kind_(kind) {
    setWindowTitle(tr("Edit %1 Diagram").arg(diagramKindLabel(kind)));
    resize(1080, 640);
    auto* lay = new QVBoxLayout(this);

    auto* top = new QHBoxLayout;
    auto* title = new QLabel(QStringLiteral("<b>%1</b>").arg(diagramKindLabel(kind)), this);
    templates_ = new QComboBox(this);
    templates_->addItem(tr("Insert template…"));
    if (kind == DiagramKind::PlantUml)
        for (const auto& t : kPlantUml)
            templates_->addItem(QString::fromUtf8(t.name), QString::fromUtf8(t.source));
    else
        for (const auto& t : kMermaid)
            templates_->addItem(QString::fromUtf8(t.name), QString::fromUtf8(t.source));
    top->addWidget(title);
    top->addStretch();
    top->addWidget(templates_);
    lay->addLayout(top);

    auto* split = new QSplitter(this);
    editor_ = new QPlainTextEdit(this);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSizeF(QFont().pointSizeF() + 1);
    editor_->setFont(mono);
    editor_->setPlainText(source);
    editor_->setTabStopDistance(QFontMetricsF(mono).horizontalAdvance(u' ') * 4);
    editor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    split->addWidget(editor_);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setAlignment(Qt::AlignCenter);
    preview_ = new QLabel(this);
    preview_->setAlignment(Qt::AlignCenter);
    preview_->setMinimumSize(200, 200);
    scroll->setWidget(preview_);
    split->addWidget(scroll);
    split->setSizes({440, 640});
    lay->addWidget(split, 1);

    status_ = new QLabel(this);
    status_->setWordWrap(true);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addWidget(status_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    auto* done = buttons->addButton(tr("Done"), QDialogButtonBox::AcceptRole);
    done->setProperty("accent", true);
    done->setDefault(true);
    lay->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(templates_, &QComboBox::activated, this, [this](int i) {
        if (i <= 0)
            return;
        editor_->setPlainText(templates_->itemData(i).toString());
        templates_->setCurrentIndex(0);
    });
    debounce_.setSingleShot(true);
    debounce_.setInterval(350);
    connect(&debounce_, &QTimer::timeout, this, &DiagramEditorDialog::renderPreview);
    connect(editor_, &QPlainTextEdit::textChanged, this, &DiagramEditorDialog::schedulePreview);
    renderPreview();
}

QString DiagramEditorDialog::source() const {
    return editor_->toPlainText();
}

void DiagramEditorDialog::schedulePreview() {
    status_->setText(tr("Rendering…"));
    debounce_.start();
}

void DiagramEditorDialog::renderPreview() {
    const int gen = ++generation_;
    QPointer<DiagramEditorDialog> self(this);
    status_->setText(tr("Rendering…"));
    service_.preview(kind_, source(), [self, gen](const QImage& img, const QString& error) {
        if (!self || gen != self->generation_)
            return;
        if (!error.isEmpty()) {
            self->status_->setStyleSheet(QStringLiteral("color: #d73a3a"));
            self->status_->setText(error);
            return;
        }
        self->status_->setStyleSheet(QStringLiteral("color: %1").arg(Theme::palette().tertiaryText.name()));
        self->status_->setText(tr("Preview is rendered locally."));
        self->preview_->setPixmap(QPixmap::fromImage(img));
    });
}

}  // namespace zn
