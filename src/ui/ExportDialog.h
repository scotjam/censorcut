#pragma once

#include "core/Project.h"

#include <QDialog>
#include <QString>

class QLabel;
class QProgressBar;
class QPushButton;
class QRadioButton;

namespace censorcut {

class ExportController;

class ExportDialog : public QDialog {
    Q_OBJECT
public:
    explicit ExportDialog(QWidget* parent = nullptr);

    /// Project to export. Copies in by value. Must be called before exec().
    void setProject(const Project& project);

private slots:
    void onExportClicked();
    void onCancelClicked();
    void onProgress(double fraction);
    void onPhase(const QString& label);
    void onCompleted(const QString& outputPath);
    void onFailed(const QString& reason);

private:
    void rebuildOutputPath();
    void setRunningUi(bool running);

    Project       m_project;
    bool          m_haveProject     = false;
    QString       m_outputPath;
    QString       m_completedPath;

    QLabel*       m_sourceLabel    = nullptr;
    QLabel*       m_outputLabel    = nullptr;
    QRadioButton* m_radioAccurate  = nullptr;
    QRadioButton* m_radioFast      = nullptr;
    QPushButton*  m_exportBtn      = nullptr;
    QPushButton*  m_cancelBtn      = nullptr;
    QPushButton*  m_revealBtn      = nullptr;
    QPushButton*  m_closeBtn       = nullptr;
    QProgressBar* m_progress       = nullptr;
    QLabel*       m_phaseLabel     = nullptr;

    ExportController* m_controller = nullptr;
};

} // namespace censorcut
