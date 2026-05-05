#pragma once

#include <QHash>
#include <QWidget>

class QVBoxLayout;
class QFrame;
class QLabel;
class QProgressBar;
class QPushButton;

namespace censorcut {

class ExportQueue;

/// A scrolling list of export queue rows. Lives inside the bottom dock
/// of MainWindow. One row per job: status icon, filename, progress bar,
/// and a Cancel/Show-in-folder/Dismiss button.
class ExportQueuePanel : public QWidget {
    Q_OBJECT
public:
    explicit ExportQueuePanel(ExportQueue* queue, QWidget* parent = nullptr);

private slots:
    void onJobAdded(int jobId);
    void onJobUpdated(int jobId);
    void onJobRemoved(int jobId);

private:
    struct Row {
        QFrame*       frame    = nullptr;
        QLabel*       title    = nullptr;
        QLabel*       phase    = nullptr;
        QProgressBar* progress = nullptr;
        QPushButton*  action   = nullptr;
    };

    Row*  ensureRow(int jobId);
    void  updateRow(int jobId);
    void  revealInFolder(const QString& path);

    ExportQueue*       m_queue       = nullptr;
    QVBoxLayout*       m_jobsLayout  = nullptr;
    QLabel*            m_emptyLabel  = nullptr;
    QHash<int, Row>    m_rows;
};

} // namespace censorcut
