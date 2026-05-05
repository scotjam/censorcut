#pragma once

#include "ExportPlan.h"
#include "Project.h"

#include <QList>
#include <QObject>
#include <QString>

namespace censorcut {

class ExportController;

/// A FIFO queue of export jobs that runs sequentially in the background.
/// Owns a single ExportController and feeds it one job at a time.
/// Pending jobs can be cancelled; the running one can be killed.
class ExportQueue : public QObject {
    Q_OBJECT
public:
    enum class Status { Pending, Running, Done, Failed, Cancelled };

    struct Job {
        int           id          = -1;
        Project       project;
        QString       destination;
        ExportQuality quality     = ExportQuality::Accurate;
        Status        status      = Status::Pending;
        double        progress    = 0.0;
        QString       phaseLabel;
        /// On Done: the absolute path of the output file.
        /// On Failed: the error message.
        QString       message;
    };

    explicit ExportQueue(QObject* parent = nullptr);
    ~ExportQueue() override;

    /// Append a job. Returns its id.
    int enqueue(const Project& project, const QString& destination, ExportQuality quality);

    /// Cancel a job by id. If running, kills ffmpeg. If pending, marks
    /// Cancelled in place. No-op for already-finished jobs.
    void cancel(int jobId);

    /// Forget a finished/cancelled job (removes from the list). No-op for
    /// running or pending jobs.
    void dismiss(int jobId);

    QList<Job> jobs() const { return m_jobs; }
    int        runningJobId() const { return m_runningJobId; }

signals:
    void jobAdded(int jobId);
    void jobUpdated(int jobId);
    void jobRemoved(int jobId);

private slots:
    void onProgress(double frac);
    void onPhase(const QString& label);
    void onCompleted(const QString& outputPath);
    void onFailed(const QString& reason);

private:
    void startNext();
    int  indexOfJob(int jobId) const;

    QList<Job>        m_jobs;
    int               m_runningJobId = -1;
    int               m_nextJobId    = 1;
    ExportController* m_controller   = nullptr;
};

} // namespace censorcut
