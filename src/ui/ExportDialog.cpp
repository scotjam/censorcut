#include "ExportDialog.h"

#include "core/ExportController.h"
#include "core/ExportPlan.h"

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QUrl>
#include <QVBoxLayout>

namespace censorcut {

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

    auto* qGroup = new QGroupBox(tr("Quality"), this);
    auto* qLayout = new QVBoxLayout(qGroup);
    m_radioAccurate = new QRadioButton(
        tr("Frame-accurate (re-encode each kept segment, slower)"), qGroup);
    m_radioFast = new QRadioButton(
        tr("Fast (keyframe-aligned stream copy, near-instant; cuts may be off by up to a GOP)"), qGroup);
    m_radioAccurate->setChecked(true);
    qLayout->addWidget(m_radioAccurate);
    qLayout->addWidget(m_radioFast);

    m_phaseLabel = new QLabel(this);
    m_phaseLabel->setStyleSheet(QStringLiteral("color:#888;"));
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 1000);
    m_progress->setTextVisible(true);
    m_progress->setVisible(false);
    m_phaseLabel->setVisible(false);

    m_exportBtn = new QPushButton(tr("Export"), this);
    m_exportBtn->setDefault(true);
    m_cancelBtn = new QPushButton(tr("Cancel"), this);
    m_cancelBtn->setVisible(false);
    m_revealBtn = new QPushButton(tr("Show in Folder"), this);
    m_revealBtn->setVisible(false);
    m_closeBtn  = new QPushButton(tr("Close"), this);

    auto* btns = new QHBoxLayout;
    btns->addStretch(1);
    btns->addWidget(m_revealBtn);
    btns->addWidget(m_cancelBtn);
    btns->addWidget(m_exportBtn);
    btns->addWidget(m_closeBtn);

    auto* main = new QVBoxLayout(this);
    main->addLayout(form);
    main->addWidget(qGroup);
    main->addWidget(m_phaseLabel);
    main->addWidget(m_progress);
    main->addLayout(btns);

    m_controller = new ExportController(this);
    connect(m_controller, &ExportController::progressChanged,
            this, &ExportDialog::onProgress);
    connect(m_controller, &ExportController::phaseChanged,
            this, &ExportDialog::onPhase);
    connect(m_controller, &ExportController::completed,
            this, &ExportDialog::onCompleted);
    connect(m_controller, &ExportController::failed,
            this, &ExportDialog::onFailed);

    connect(m_exportBtn, &QPushButton::clicked, this, &ExportDialog::onExportClicked);
    connect(m_cancelBtn, &QPushButton::clicked, this, &ExportDialog::onCancelClicked);
    connect(m_closeBtn,  &QPushButton::clicked, this, &QDialog::reject);
    connect(m_revealBtn, &QPushButton::clicked, this, [this]{
        if (m_completedPath.isEmpty()) return;
#ifdef Q_OS_WIN
        QProcess::startDetached(QStringLiteral("explorer.exe"),
                                {QStringLiteral("/select,"),
                                 QDir::toNativeSeparators(m_completedPath)});
#else
        QDesktopServices::openUrl(
            QUrl::fromLocalFile(QFileInfo(m_completedPath).absolutePath()));
#endif
    });
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

void ExportDialog::onExportClicked()
{
    if (!m_haveProject) {
        QMessageBox::warning(this, tr("Nothing to export"),
                             tr("Open a movie before exporting."));
        return;
    }
    // Refuse silently to overwrite — confirm first.
    if (QFileInfo::exists(m_outputPath)) {
        const auto reply = QMessageBox::question(
            this, tr("Replace existing file?"),
            tr("\"%1\" already exists.\n\nReplace it?").arg(m_outputPath),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (reply != QMessageBox::Yes) return;
    }

    const ExportQuality quality = m_radioFast->isChecked()
        ? ExportQuality::Fast : ExportQuality::Accurate;

    setRunningUi(true);
    m_progress->setValue(0);
    m_phaseLabel->setText(tr("Preparing…"));

    if (!m_controller->start(m_project, m_outputPath, quality)) {
        // start() emits failed() which will reset the UI via onFailed.
    }
}

void ExportDialog::onCancelClicked()
{
    if (m_controller->isRunning()) m_controller->cancel();
}

void ExportDialog::onProgress(double fraction)
{
    m_progress->setValue(int(fraction * 1000));
}

void ExportDialog::onPhase(const QString& label)
{
    m_phaseLabel->setText(label);
}

void ExportDialog::onCompleted(const QString& outputPath)
{
    m_completedPath = outputPath;
    setRunningUi(false);
    m_revealBtn->setVisible(true);
    m_progress->setValue(1000);
    m_phaseLabel->setText(tr("Done — wrote %1").arg(QFileInfo(outputPath).fileName()));
    m_exportBtn->setVisible(false);
}

void ExportDialog::onFailed(const QString& reason)
{
    setRunningUi(false);
    QMessageBox::warning(this, tr("Export failed"), reason);
    m_phaseLabel->setVisible(false);
    m_progress->setVisible(false);
}

void ExportDialog::setRunningUi(bool running)
{
    m_radioAccurate->setEnabled(!running);
    m_radioFast->setEnabled(!running);
    m_exportBtn->setEnabled(!running);
    m_exportBtn->setVisible(!running);
    m_closeBtn->setEnabled(!running);
    m_cancelBtn->setVisible(running);
    m_progress->setVisible(running || !m_completedPath.isEmpty());
    m_phaseLabel->setVisible(running || !m_completedPath.isEmpty());
}

} // namespace censorcut
