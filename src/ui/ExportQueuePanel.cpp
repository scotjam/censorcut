#include "ExportQueuePanel.h"

#include "core/ExportQueue.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

namespace censorcut {

namespace {

QString statusIcon(ExportQueue::Status s)
{
    switch (s) {
        case ExportQueue::Status::Pending:   return QStringLiteral("⌛");
        case ExportQueue::Status::Running:   return QStringLiteral("▶");
        case ExportQueue::Status::Done:      return QStringLiteral("✓");
        case ExportQueue::Status::Failed:    return QStringLiteral("✗");
        case ExportQueue::Status::Cancelled: return QStringLiteral("⊘");
    }
    return QString();
}

} // namespace

ExportQueuePanel::ExportQueuePanel(ExportQueue* queue, QWidget* parent)
    : QWidget(parent), m_queue(queue)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(8, 4, 8, 4);
    outer->setSpacing(2);

    m_emptyLabel = new QLabel(tr("No exports running. File → Export Censored Cut… (Ctrl+E)"), this);
    m_emptyLabel->setStyleSheet(QStringLiteral("color:#888;"));
    outer->addWidget(m_emptyLabel);

    m_jobsLayout = new QVBoxLayout;
    m_jobsLayout->setContentsMargins(0, 0, 0, 0);
    m_jobsLayout->setSpacing(2);
    outer->addLayout(m_jobsLayout);
    outer->addStretch(1);

    connect(m_queue, &ExportQueue::jobAdded,   this, &ExportQueuePanel::onJobAdded);
    connect(m_queue, &ExportQueue::jobUpdated, this, &ExportQueuePanel::onJobUpdated);
    connect(m_queue, &ExportQueue::jobRemoved, this, &ExportQueuePanel::onJobRemoved);
}

ExportQueuePanel::Row* ExportQueuePanel::ensureRow(int jobId)
{
    auto it = m_rows.find(jobId);
    if (it != m_rows.end()) return &it.value();

    Row row;
    row.frame = new QFrame(this);
    row.frame->setFrameShape(QFrame::StyledPanel);
    auto* h = new QHBoxLayout(row.frame);
    h->setContentsMargins(6, 2, 6, 2);

    row.title = new QLabel(row.frame);
    row.phase = new QLabel(row.frame);
    row.phase->setStyleSheet(QStringLiteral("color:#888;"));
    row.progress = new QProgressBar(row.frame);
    row.progress->setRange(0, 1000);
    row.progress->setMaximumWidth(220);
    row.progress->setTextVisible(true);
    row.action = new QPushButton(row.frame);
    // Don't trap focus when clicked — keeps the transport keys working
    // after interacting with the export queue.
    row.action->setFocusPolicy(Qt::NoFocus);

    h->addWidget(row.title);
    h->addWidget(row.phase, /*stretch=*/1);
    h->addWidget(row.progress);
    h->addWidget(row.action);

    m_jobsLayout->addWidget(row.frame);
    m_emptyLabel->hide();

    m_rows.insert(jobId, row);
    return &m_rows[jobId];
}

void ExportQueuePanel::onJobAdded(int jobId)
{
    ensureRow(jobId);
    updateRow(jobId);
}

void ExportQueuePanel::onJobUpdated(int jobId)
{
    updateRow(jobId);
}

void ExportQueuePanel::onJobRemoved(int jobId)
{
    auto it = m_rows.find(jobId);
    if (it == m_rows.end()) return;
    it.value().frame->deleteLater();
    m_rows.erase(it);
    if (m_rows.isEmpty()) m_emptyLabel->show();
}

void ExportQueuePanel::updateRow(int jobId)
{
    auto* row = ensureRow(jobId);
    const auto& jobs = m_queue->jobs();
    const ExportQueue::Job* job = nullptr;
    for (const auto& j : jobs) if (j.id == jobId) { job = &j; break; }
    if (!job) return;

    row->title->setText(QStringLiteral("%1  %2")
                            .arg(statusIcon(job->status),
                                 QFileInfo(job->destination).fileName()));
    row->phase->setText(job->phaseLabel.isEmpty() && job->status == ExportQueue::Status::Pending
                            ? tr("queued") : job->phaseLabel);

    const bool showProgress = (job->status == ExportQueue::Status::Running)
                              || (job->status == ExportQueue::Status::Done && job->progress >= 1.0);
    row->progress->setVisible(showProgress);
    row->progress->setValue(int(job->progress * 1000));

    // Action button: Cancel for pending/running, Show-in-Folder for Done,
    // Dismiss for terminal-non-running jobs.
    QObject::disconnect(row->action, nullptr, nullptr, nullptr);
    switch (job->status) {
        case ExportQueue::Status::Pending:
        case ExportQueue::Status::Running:
            row->action->setText(tr("Cancel"));
            connect(row->action, &QPushButton::clicked, this,
                    [this, jobId]{ m_queue->cancel(jobId); });
            break;
        case ExportQueue::Status::Done: {
            row->action->setText(tr("Show in Folder"));
            const QString path = job->message;
            connect(row->action, &QPushButton::clicked, this,
                    [this, path]{ revealInFolder(path); });
            break;
        }
        case ExportQueue::Status::Failed:
        case ExportQueue::Status::Cancelled:
            row->action->setText(tr("Dismiss"));
            row->frame->setToolTip(job->message);
            connect(row->action, &QPushButton::clicked, this,
                    [this, jobId]{ m_queue->dismiss(jobId); });
            break;
    }
}

void ExportQueuePanel::revealInFolder(const QString& path)
{
    if (path.isEmpty()) return;
#ifdef Q_OS_WIN
    QProcess::startDetached(QStringLiteral("explorer.exe"),
                            {QStringLiteral("/select,"),
                             QDir::toNativeSeparators(path)});
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}

} // namespace censorcut
