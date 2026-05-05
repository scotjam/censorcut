#include "ExportDialog.h"

#include <QCheckBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QVBoxLayout>

namespace censorcut {

namespace {
constexpr const char* kAutoSaveKey = "exports/autoSaveOnStart";
} // namespace

ExportDialog::ExportDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Export Censored Cut"));
    setMinimumWidth(560);

    auto* form = new QFormLayout;
    m_sourceLabel = new QLabel(this);
    m_sourceLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_sourceLabel->setWordWrap(true);
    m_outputLabel = new QLabel(this);
    m_outputLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_outputLabel->setWordWrap(true);
    form->addRow(tr("Source"), m_sourceLabel);
    form->addRow(tr("Output"), m_outputLabel);

    auto* qGroup  = new QGroupBox(tr("Quality"), this);
    auto* qLayout = new QVBoxLayout(qGroup);
    m_radioAccurate = new QRadioButton(
        tr("Frame-accurate (re-encode each kept segment, slower)"), qGroup);
    m_radioFast = new QRadioButton(
        tr("Fast (keyframe-aligned stream copy, near-instant; cuts may be off by up to a GOP)"), qGroup);
    m_radioAccurate->setChecked(true);
    qLayout->addWidget(m_radioAccurate);
    qLayout->addWidget(m_radioFast);

    QSettings settings;
    m_autoSaveCheck = new QCheckBox(tr("Auto-save markers when starting an export"), this);
    m_autoSaveCheck->setChecked(settings.value(QString::fromLatin1(kAutoSaveKey), false).toBool());
    m_autoSaveCheck->setToolTip(tr("If checked, marker changes are saved to the sidecar JSON before "
                                   "the export starts. Otherwise you'll get a save prompt when "
                                   "there are unsaved changes."));
    connect(m_autoSaveCheck, &QCheckBox::toggled, this, [](bool on) {
        QSettings s;
        s.setValue(QString::fromLatin1(kAutoSaveKey), on);
    });

    auto* addBtn   = new QPushButton(tr("Add to Export Queue"), this);
    addBtn->setDefault(true);
    auto* closeBtn = new QPushButton(tr("Cancel"), this);

    auto* btns = new QHBoxLayout;
    btns->addStretch(1);
    btns->addWidget(closeBtn);
    btns->addWidget(addBtn);

    auto* main = new QVBoxLayout(this);
    main->addLayout(form);
    main->addWidget(qGroup);
    main->addWidget(m_autoSaveCheck);
    main->addLayout(btns);

    connect(addBtn,   &QPushButton::clicked, this, &ExportDialog::onAddToQueue);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
}

void ExportDialog::setProject(const Project& project)
{
    m_project     = project;
    m_haveProject = true;
    m_sourceLabel->setText(project.sourceFile);
    rebuildOutputPath();
}

void ExportDialog::rebuildOutputPath()
{
    if (!m_haveProject) return;
    m_outputPath = Project::censoredOutputPathFor(m_project.sourceFile,
                                                  m_project.activeProfile.minAge);
    m_outputLabel->setText(m_outputPath);
}

ExportQuality ExportDialog::quality() const
{
    return m_radioFast->isChecked() ? ExportQuality::Fast : ExportQuality::Accurate;
}

bool ExportDialog::autoSaveOnStart() const
{
    return m_autoSaveCheck->isChecked();
}

void ExportDialog::onAddToQueue()
{
    if (!m_haveProject) {
        QMessageBox::warning(this, tr("Nothing to export"), tr("Open a movie first."));
        return;
    }
    if (QFileInfo::exists(m_outputPath)) {
        const auto reply = QMessageBox::question(
            this, tr("Replace existing file?"),
            tr("\"%1\" already exists.\n\nReplace it when this export runs?").arg(m_outputPath),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (reply != QMessageBox::Yes) return;
    }
    accept();
}

} // namespace censorcut
