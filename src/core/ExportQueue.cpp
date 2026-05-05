#include "ExportQueue.h"

#include "ExportController.h"

namespace censorcut {

ExportQueue::ExportQueue(QObject* parent)
    : QObject(parent)
{
    m_controller = new ExportController(this);
    connect(m_controller, &ExportController::progressChanged,
            this, &ExportQueue::onProgress);
    connect(m_controller, &ExportController::phaseChanged,
            this, &ExportQueue::onPhase);
    connect(m_controller, &ExportController::completed,
            this, &ExportQueue::onCompleted);
    connect(m_controller, &ExportController::failed,
            this, &ExportQueue::onFailed);
}

ExportQueue::~ExportQueue() = default;

int ExportQueue::enqueue(const Project& project,
                         const QString& destination,
                         ExportQuality quality)
{
    Job job;
    job.id          = m_nextJobId++;
    job.project     = project;
    job.destination = destination;
    job.quality     = quality;
    m_jobs.append(job);
    emit jobAdded(job.id);
    if (m_runningJobId < 0) startNext();
    return job.id;
}

void ExportQueue::cancel(int jobId)
{
    const int idx = indexOfJob(jobId);
    if (idx < 0) return;
    Job& job = m_jobs[idx];
    if (job.status == Status::Running) {
        m_controller->cancel();
        // onFailed("Cancelled.") will fire and finalize the state.
    } else if (job.status == Status::Pending) {
        job.status = Status::Cancelled;
        job.message = QStringLiteral("Cancelled before start.");
        emit jobUpdated(jobId);
    }
}

void ExportQueue::dismiss(int jobId)
{
    const int idx = indexOfJob(jobId);
    if (idx < 0) return;
    const Job& job = m_jobs[idx];
    if (job.status == Status::Running || job.status == Status::Pending) return;
    m_jobs.removeAt(idx);
    emit jobRemoved(jobId);
}

void ExportQueue::startNext()
{
    int nextIdx = -1;
    for (int i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs[i].status == Status::Pending) { nextIdx = i; break; }
    }
    if (nextIdx < 0) return;
    Job& job = m_jobs[nextIdx];
    job.status     = Status::Running;
    m_runningJobId = job.id;
    emit jobUpdated(job.id);

    if (!m_controller->start(job.project, job.destination, job.quality)) {
        // ExportController emits failed() which feeds back into onFailed.
    }
}

void ExportQueue::onProgress(double frac)
{
    if (m_runningJobId < 0) return;
    const int idx = indexOfJob(m_runningJobId);
    if (idx < 0) return;
    m_jobs[idx].progress = frac;
    emit jobUpdated(m_runningJobId);
}

void ExportQueue::onPhase(const QString& label)
{
    if (m_runningJobId < 0) return;
    const int idx = indexOfJob(m_runningJobId);
    if (idx < 0) return;
    m_jobs[idx].phaseLabel = label;
    emit jobUpdated(m_runningJobId);
}

void ExportQueue::onCompleted(const QString& outputPath)
{
    if (m_runningJobId < 0) return;
    const int finishedId = m_runningJobId;
    const int idx = indexOfJob(finishedId);
    if (idx >= 0) {
        m_jobs[idx].status   = Status::Done;
        m_jobs[idx].progress = 1.0;
        m_jobs[idx].message  = outputPath;
        emit jobUpdated(finishedId);
    }
    m_runningJobId = -1;
    startNext();
}

void ExportQueue::onFailed(const QString& reason)
{
    if (m_runningJobId < 0) return;
    const int finishedId = m_runningJobId;
    const int idx = indexOfJob(finishedId);
    if (idx >= 0) {
        const bool wasCancel = reason.contains(QStringLiteral("Cancelled"), Qt::CaseInsensitive);
        m_jobs[idx].status  = wasCancel ? Status::Cancelled : Status::Failed;
        m_jobs[idx].message = reason;
        emit jobUpdated(finishedId);
    }
    m_runningJobId = -1;
    startNext();
}

int ExportQueue::indexOfJob(int jobId) const
{
    for (int i = 0; i < m_jobs.size(); ++i)
        if (m_jobs[i].id == jobId) return i;
    return -1;
}

} // namespace censorcut
